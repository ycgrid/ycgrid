// SPDX-License-Identifier: Apache-2.0
// ycg.cpp - .ycg reading and writing (spec in ycg.h and docs/ycg-format.md)
#include "io/ycg.h"

#include "io/crypto.h"
#include "core/i18n.h"

#include "core/decimal.h"
#include "engine/snapshot.h"
#include "engine/tile_iter.h"
#include "formula/parser.h"
#include "io/msgpack.h"
#include "io/msgvalue.h"

#include <windows.h>
#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>

namespace yc::io {

using core::Rect;
using core::Value;
using model::Sheet;

namespace {

// ---- Chunk identifiers and current versions ----
constexpr uint32_t kMETA = FourCC("META");
constexpr uint32_t kWKBK = FourCC("WKBK");
constexpr uint32_t kSTYL = FourCC("STYL");
constexpr uint32_t kSHET = FourCC("SHET");
constexpr uint32_t kFMLA = FourCC("FMLA");
constexpr uint32_t kCELL = FourCC("CELL");
constexpr uint32_t kNAME = FourCC("NAME");
constexpr uint32_t kKVST = FourCC("KVST");
constexpr uint32_t kPROP = FourCC("PROP");
constexpr uint32_t kPANL = FourCC("PANL");
constexpr uint32_t kCONT = FourCC("CONT");
constexpr uint32_t kTHEM = FourCC("THEM");
constexpr uint32_t kCSTY = FourCC("CSTY");
constexpr uint32_t kEXTL = FourCC("EXTL");
constexpr uint32_t kLAYT = FourCC("LAYT");
constexpr uint32_t kBIND = FourCC("BIND");
constexpr uint32_t kSAFE = FourCC("SAFE");
constexpr uint32_t kNBOK = FourCC("NBOK");

struct KnownChunk {
  uint32_t fourcc;
  uint16_t version;  // version written by this program (also the highest it can read)
  uint16_t flags;
};
constexpr KnownChunk kKnown[] = {
    {kMETA, 1, kChunkPreserve}, {kWKBK, 1, kChunkCritical}, {kSTYL, 1, kChunkCritical}, {kSHET, 1, kChunkCritical},
    {kFMLA, 1, kChunkCritical}, {kCELL, 1, kChunkCritical}, {kNAME, 1, kChunkCritical}, {kKVST, 1, kChunkCritical},
    {kPROP, 1, kChunkCritical}, {kPANL, 1, kChunkPreserve}, {kCONT, 1, kChunkPreserve}, {kLAYT, 1, kChunkPreserve}, {kTHEM, 1, kChunkPreserve}, {kCSTY, 1, kChunkPreserve}, {kEXTL, 1, kChunkPreserve},
    {kBIND, 1, kChunkPreserve}, {kSAFE, 1, kChunkPreserve},
    {kNBOK, 1, kChunkPreserve},
};
const KnownChunk* Known(uint32_t fourcc) {
  for (const auto& k : kKnown) {
    if (k.fourcc == fourcc) return &k;
  }
  return nullptr;
}

std::string FourCCText(uint32_t f) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) s[static_cast<size_t>(i)] = static_cast<char>((f >> (8 * i)) & 0xFF);
  return s;
}

// ---- Helpers: little-endian, varint ----
void PutLE(std::string& b, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b += static_cast<char>((v >> (8 * i)) & 0xFF);
}
uint64_t GetLE(std::string_view b, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(b[at + static_cast<size_t>(i)]);
  return v;
}
void PutVar(std::string& b, uint64_t v) {
  while (v >= 0x80) {
    b += static_cast<char>((v & 0x7F) | 0x80);
    v >>= 7;
  }
  b += static_cast<char>(v);
}
bool GetVar(std::string_view b, size_t* p, uint64_t* out) {
  uint64_t v = 0;
  int shift = 0;
  while (*p < b.size() && shift < 64) {
    const uint8_t c = static_cast<uint8_t>(b[(*p)++]);
    v |= static_cast<uint64_t>(c & 0x7F) << shift;
    if (!(c & 0x80)) {
      *out = v;
      return true;
    }
    shift += 7;
  }
  return false;
}

// ---- Value encoding (type codes shared by cells and msgpack; numbers never change meaning) ----
enum ValTag : uint8_t { kVEmpty = 0, kVNumber = 1, kVFalse = 2, kVTrue = 3, kVError = 4, kVString = 5, kVDecimal = 6 };

void PutValue(std::string& b, const Value& v) {
  switch (v.type()) {
    case core::VType::Number: {
      b += static_cast<char>(kVNumber);
      uint64_t u;
      const double d = v.number();
      std::memcpy(&u, &d, 8);
      PutLE(b, u, 8);
      break;
    }
    case core::VType::Bool: b += static_cast<char>(v.boolean() ? kVTrue : kVFalse); break;
    case core::VType::Error:
      b += static_cast<char>(kVError);
      b += static_cast<char>(v.error());
      break;
    case core::VType::StrInline:
    case core::VType::StrHeap: {
      b += static_cast<char>(kVString);
      const auto s = v.str();
      PutVar(b, s.size());
      b.append(s);
      break;
    }
    case core::VType::Decimal: {
      b += static_cast<char>(kVDecimal);
      b += static_cast<char>(v.dec_scale());
      const core::Int128 m = v.dec_mantissa();
      PutLE(b, m.lo, 8);
      PutLE(b, static_cast<uint64_t>(m.hi), 8);
      break;
    }
    default: b += static_cast<char>(kVEmpty); break;
  }
}

bool GetValue(std::string_view b, size_t* p, Value* out) {
  if (*p >= b.size()) return false;
  const uint8_t tag = static_cast<uint8_t>(b[(*p)++]);
  switch (tag) {
    case kVEmpty: *out = Value(); return true;
    case kVNumber: {
      if (*p + 8 > b.size()) return false;
      const uint64_t u = GetLE(b, *p, 8);
      *p += 8;
      double d;
      std::memcpy(&d, &u, 8);
      *out = Value::Number(d);
      return true;
    }
    case kVFalse: *out = Value::Bool(false); return true;
    case kVTrue: *out = Value::Bool(true); return true;
    case kVError: {
      if (*p >= b.size()) return false;
      *out = Value::Error(static_cast<core::Err>(static_cast<uint8_t>(b[(*p)++])));
      return true;
    }
    case kVString: {
      uint64_t n;
      if (!GetVar(b, p, &n) || *p + n > b.size()) return false;
      *out = Value::String(b.substr(*p, n));
      *p += n;
      return true;
    }
    case kVDecimal: {
      if (*p + 17 > b.size()) return false;
      const int scale = static_cast<uint8_t>(b[(*p)++]);
      core::Int128 m;
      m.lo = GetLE(b, *p, 8);
      m.hi = static_cast<int64_t>(GetLE(b, *p + 8, 8));
      *p += 16;
      *out = Value::DecimalRaw(m, scale);
      return true;
    }
    default: return false;
  }
}

void WriteMsgValue(MsgWriter& w, const Value& v) {
  std::string tmp;
  PutValue(tmp, v);
  w.Bin(tmp);
}
bool ReadMsgValue(MsgReader& r, Value* v) {
  std::string tmp;
  if (!r.Bin(&tmp)) return false;
  size_t p = 0;
  return GetValue(tmp, &p, v);
}

// ---- Styles ----
void WriteStyle(MsgWriter& w, const model::Style& s, const model::StyleTable& t) {
  const model::Style d;
  uint32_t n = 0;
  auto count = [&](bool c) { n += c ? 1 : 0; };
  count(s.font_name != 0);
  count(s.font_size != d.font_size);
  count(s.font_color != d.font_color);
  count(s.bold);
  count(s.italic);
  count(s.strike);
  count(s.underline != d.underline);
  count(s.fill_color != d.fill_color);
  bool any_border = false;
  for (const auto& b : s.border) any_border = any_border || b.style != model::LineStyle::None;
  count(any_border);
  count(s.halign != d.halign);
  count(s.valign != d.valign);
  count(s.indent != 0);
  count(s.wrap);
  count(s.shrink);
  count(s.rotation != 0);
  count(s.numfmt != 0);
  count(!s.locked);
  count(s.hidden);
  count(s.pattern != d.pattern);
  count(s.pattern_color != d.pattern_color);
  count(s.gradient != d.gradient);
  count(s.gradient_angle != 0);
  count(s.gradient_color2 != d.gradient_color2);
  count(s.script != 0);
  count(s.cell_style != 0);
  count(s.justify_last_line);
  count(s.reading_order != 0);
  w.Map(n);
  if (s.font_name) w.KStr(1, t.Str(s.font_name));
  if (s.font_size != d.font_size) w.KDouble(2, s.font_size);
  if (s.font_color != d.font_color) w.KInt(3, s.font_color);
  if (s.bold) w.KBool(4, true);
  if (s.italic) w.KBool(5, true);
  if (s.strike) w.KBool(6, true);
  if (s.underline != d.underline) w.KInt(7, static_cast<int>(s.underline));
  if (s.fill_color != d.fill_color) w.KInt(8, s.fill_color);
  if (any_border) {
    w.Int(9);
    w.Array(model::kEdgeCount);
    for (const auto& b : s.border) {
      w.Array(2);
      w.Int(static_cast<int>(b.style));
      w.Int(b.color);
    }
  }
  if (s.halign != d.halign) w.KInt(10, static_cast<int>(s.halign));
  if (s.valign != d.valign) w.KInt(11, static_cast<int>(s.valign));
  if (s.indent) w.KInt(12, s.indent);
  if (s.wrap) w.KBool(13, true);
  if (s.shrink) w.KBool(14, true);
  if (s.rotation) w.KInt(15, s.rotation);
  if (s.numfmt) w.KStr(16, t.Str(s.numfmt));
  if (!s.locked) w.KBool(17, false);
  if (s.hidden) w.KBool(18, true);
  if (s.pattern != d.pattern) w.KInt(19, static_cast<int>(s.pattern));
  if (s.pattern_color != d.pattern_color) w.KInt(20, s.pattern_color);
  if (s.gradient != d.gradient) w.KInt(21, static_cast<int>(s.gradient));
  if (s.gradient_angle) w.KInt(22, s.gradient_angle);
  if (s.gradient_color2 != d.gradient_color2) w.KInt(23, s.gradient_color2);
  if (s.script) w.KInt(24, s.script);
  if (s.cell_style) w.KStr(25, t.Str(s.cell_style));
  if (s.justify_last_line) w.KBool(26, true);
  if (s.reading_order) w.KInt(27, s.reading_order);
}

bool ReadStyle(MsgReader& r, model::Style* s, model::StyleTable& t) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1: {
        std::string v;
        ok = r.Str(&v);
        s->font_name = t.InternString(v);
        break;
      }
      case 2: {
        double v;
        ok = r.Double(&v);
        s->font_size = static_cast<float>(v);
        break;
      }
      case 3: ok = r.IntAs(&s->font_color); break;
      case 4: ok = r.Bool(&s->bold); break;
      case 5: ok = r.Bool(&s->italic); break;
      case 6: ok = r.Bool(&s->strike); break;
      case 7: {
        int64_t v;
        ok = r.Int(&v);
        s->underline = static_cast<model::Underline>(v);
        break;
      }
      case 8: ok = r.IntAs(&s->fill_color); break;
      case 9: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t e = 0; ok && e < m; ++e) {
          uint32_t two = 0;
          int64_t st = 0, col = 0;
          ok = r.Array(&two) && two == 2 && r.Int(&st) && r.Int(&col);
          if (ok && e < model::kEdgeCount) s->border[e] = model::BorderSide{static_cast<model::LineStyle>(st), static_cast<uint32_t>(col)};
        }
        break;
      }
      case 10: {
        int64_t v;
        ok = r.Int(&v);
        s->halign = static_cast<model::HAlign>(v);
        break;
      }
      case 11: {
        int64_t v;
        ok = r.Int(&v);
        s->valign = static_cast<model::VAlign>(v);
        break;
      }
      case 12: ok = r.IntAs(&s->indent); break;
      case 13: ok = r.Bool(&s->wrap); break;
      case 14: ok = r.Bool(&s->shrink); break;
      case 15: ok = r.IntAs(&s->rotation); break;
      case 16: {
        std::string v;
        ok = r.Str(&v);
        s->numfmt = v == "General" ? 0 : t.InternString(v);
        break;
      }
      case 17: ok = r.Bool(&s->locked); break;
      case 18: ok = r.Bool(&s->hidden); break;
      case 19: {
        int64_t v;
        ok = r.Int(&v);
        if (v >= 0 && v < static_cast<int64_t>(model::FillPattern::Count)) s->pattern = static_cast<model::FillPattern>(v);
        break;
      }
      case 20: ok = r.IntAs(&s->pattern_color); break;
      case 21: {
        int64_t v;
        ok = r.Int(&v);
        if (v >= 0 && v <= 3) s->gradient = static_cast<model::FillGradient>(v);
        break;
      }
      case 22: ok = r.IntAs(&s->gradient_angle); break;
      case 23: ok = r.IntAs(&s->gradient_color2); break;
      case 24: ok = r.IntAs(&s->script); break;
      case 25: {
        std::string v;
        ok = r.Str(&v);
        s->cell_style = v.empty() ? 0 : t.InternString(v);
        break;
      }
      case 26: ok = r.Bool(&s->justify_last_line); break;
      case 27: {
        int64_t v;
        ok = r.Int(&v);
        if (v >= 0 && v <= 2) s->reading_order = static_cast<uint8_t>(v);
        break;
      }
      default: ok = r.Skip(); break;  // fields from a newer version: skip
    }
    if (!ok) return false;
  }
  return true;
}

// ---- Writing ----
struct ChunkOut {
  uint32_t fourcc;
  uint16_t version;
  uint16_t flags;
  std::string data;
};

void WriteDims(MsgWriter& w, const model::DimIndex& d) {
  const auto e = d.Entries();
  w.Array(4);
  w.Int(d.default_size());
  w.Array(static_cast<uint32_t>(e.size()));
  for (const auto& x : e) {
    w.Array(3);
    w.Int(x.index);
    w.Int(x.size);
    w.Int(x.saved);
  }
  // outline levels [[index, level]] (older versions skip the third element)
  w.Array(static_cast<uint32_t>(d.Levels().size()));
  for (const auto& [i, l] : d.Levels()) {
    w.Array(2);
    w.Int(i);
    w.Int(l);
  }
  // the summary lines of collapsed groups (older versions skip the fourth element)
  w.Array(static_cast<uint32_t>(d.Collapsed().size()));
  for (const int64_t i : d.Collapsed()) w.Int(i);
}

bool ReadDims(MsgReader& r, model::DimIndex* d) {
  uint32_t two, n;
  int64_t def;
  if (!r.Array(&two) || two < 2 || !r.Int(&def) || !r.Array(&n)) return false;
  std::vector<model::DimIndex::Entry> e;
  e.reserve(std::min<uint32_t>(n, 1u << 16));  // a damaged count must not ask for gigabytes
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t three;
    int64_t idx, sz, saved;
    if (!r.Array(&three) || three < 3 || !r.Int(&idx) || !r.Int(&sz) || !r.Int(&saved)) return false;
    for (uint32_t k = 3; k < three; ++k) r.Skip();
    e.push_back({idx, static_cast<int32_t>(sz), static_cast<int32_t>(saved)});
  }
  std::map<int64_t, uint8_t> levels;
  if (two >= 3) {
    uint32_t m;
    if (!r.Array(&m)) return false;
    for (uint32_t i = 0; i < m; ++i) {
      uint32_t k2;
      int64_t idx = 0, lvl = 0;
      if (!r.Array(&k2) || k2 < 2 || !r.Int(&idx) || !r.Int(&lvl)) return false;
      for (uint32_t k = 2; k < k2; ++k) r.Skip();
      if (lvl > 0) levels[idx] = static_cast<uint8_t>(std::min<int64_t>(lvl, model::DimIndex::kMaxOutline));
    }
  }
  std::set<int64_t> collapsed;
  if (two >= 4) {
    uint32_t m;
    if (!r.Array(&m)) return false;
    for (uint32_t i = 0; i < m; ++i) {
      int64_t idx = 0;
      if (!r.Int(&idx)) return false;
      if (idx >= 0) collapsed.insert(idx);
    }
  }
  for (uint32_t k = 4; k < two; ++k) r.Skip();
  d->Load(static_cast<int32_t>(def), e);
  d->LoadLevels(std::move(levels));
  d->LoadCollapsed(std::move(collapsed));
  return true;
}

void WriteRect(MsgWriter& w, const Rect& r) {
  w.Array(4);
  w.Int(r.r0);
  w.Int(r.c0);
  w.Int(r.r1);
  w.Int(r.c1);
}
bool ReadRect(MsgReader& r, Rect* out) {
  uint32_t n;
  int64_t a, b, c, d;
  if (!r.Array(&n) || n < 4 || !r.Int(&a) || !r.Int(&b) || !r.Int(&c) || !r.Int(&d)) return false;
  for (uint32_t k = 4; k < n; ++k) r.Skip();
  *out = Rect{static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c), static_cast<uint32_t>(d)};
  return true;
}

void WriteStringMap(MsgWriter& w, const std::map<std::string, std::string>& m, bool values_are_msgpack) {
  w.Map(static_cast<uint32_t>(m.size()));
  for (const auto& [k, v] : m) {
    w.Str(k);
    if (values_are_msgpack) w.Raw(v.empty() ? std::string_view("\xc0", 1) : std::string_view(v));
    else w.Str(v);
  }
}
bool ReadStringMap(MsgReader& r, std::map<std::string, std::string>* m, bool values_are_msgpack) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    std::string k;
    if (!r.Str(&k)) return false;
    if (values_are_msgpack) {
      std::string_view raw;
      if (!r.RawValue(&raw)) return false;
      (*m)[k] = std::string(raw);
    } else {
      std::string v;
      if (!r.Str(&v)) return false;
      (*m)[k] = v;
    }
  }
  return true;
}

// the shape record's element #12: {1: dash, 2: begin arrow, 3: end arrow, 4: fill transparency, 5-8: margins l t r b,
// 9: wrap, 10: autofit, 11: alt text, 12-15: crop l t r b, 16: locked, 17: lock aspect, 18: rotation, 19: group,
// 20: print object, 21-22: original width / height, 23: AutoShapeType, 24: connector, 25: freeform points, 26: freeform
// flags (1 closed, 2 smooth), 27: text outline color, 28: text shadow, 29: adjustments}; followed by element #13 (nil, reserved)
void WriteShapeExtra(MsgWriter& w, const model::ShapeObj& sh) {
  const model::ShapeObj d;
  const bool pic = sh.kind == model::ShapeKind::Picture;
  uint32_t n = 0;
  n += sh.line_dash != 0;
  n += sh.begin_arrow != 0;
  n += sh.end_arrow != 0;
  n += sh.fill_transparency != 0;
  n += 4;  // margins
  n += sh.wrap != d.wrap;
  n += sh.autofit != d.autofit;
  n += !sh.alt_text.empty();
  n += 4;  // crop
  n += 1;  // locked
  n += 1;  // lock aspect
  n += sh.rotation != 0;
  n += sh.group != 0;
  n += 1;  // print object
  n += 2;  // original size
  n += sh.auto_type != 0;
  n += sh.connector != 0;
  n += !sh.path.empty() ? 2 : 0;
  n += sh.text_outline != 0;
  n += sh.text_shadow;
  n += !sh.adj.empty();
  n += sh.fx.Any() || sh.fx.preset != 0;  // 30 Shape Effects
  n += sh.on_chart != 0;                   // 31 the chart it is drawn on
  (void)pic;
  w.Map(n);
  if (sh.line_dash) w.KInt(1, sh.line_dash);
  if (sh.begin_arrow) w.KInt(2, sh.begin_arrow);
  if (sh.end_arrow) w.KInt(3, sh.end_arrow);
  if (sh.fill_transparency != 0) w.KDouble(4, sh.fill_transparency);
  w.KDouble(5, sh.margin_l);
  w.KDouble(6, sh.margin_t);
  w.KDouble(7, sh.margin_r);
  w.KDouble(8, sh.margin_b);
  if (sh.wrap != d.wrap) w.KBool(9, sh.wrap);
  if (sh.autofit != d.autofit) w.KBool(10, sh.autofit);
  if (!sh.alt_text.empty()) w.KStr(11, sh.alt_text);
  w.KDouble(12, sh.crop_l);
  w.KDouble(13, sh.crop_t);
  w.KDouble(14, sh.crop_r);
  w.KDouble(15, sh.crop_b);
  w.KBool(16, sh.locked);
  w.KBool(17, sh.lock_aspect);
  if (sh.rotation != 0) w.KDouble(18, sh.rotation);
  if (sh.group) w.KInt(19, sh.group);
  if (sh.on_chart) w.KInt(31, sh.on_chart);
  w.KBool(20, sh.print_object);
  w.KDouble(21, sh.orig_w);
  w.KDouble(22, sh.orig_h);
  if (sh.auto_type) w.KInt(23, sh.auto_type);
  if (sh.connector) w.KInt(24, sh.connector);
  if (!sh.path.empty()) {
    w.Int(25);
    w.Array(static_cast<uint32_t>(sh.path.size()));
    for (double v : sh.path) w.Double(v);
    w.KInt(26, (sh.path_closed ? 1 : 0) | (sh.path_smooth ? 2 : 0));
  }
  if (sh.text_outline) w.KInt(27, sh.text_outline);
  if (sh.text_shadow) w.KBool(28, true);
  if (!sh.adj.empty()) {
    w.Int(29);
    w.Array(static_cast<uint32_t>(sh.adj.size()));
    for (double v : sh.adj) w.Double(v);
  }
  if (sh.fx.Any() || sh.fx.preset != 0) {
    // 30 Format Shape > Effects: {1 preset, 2 theme style, 3 shadow type, 4 color, 5 transparency, 6 size, 7 blur,
    // 8 angle, 9 distance, 10-13 reflection size / transparency / blur / distance, 14-16 glow size / color /
    // transparency, 17 soft edges, 18-23 bevel top / bottom type, top w / h, bottom w / h, 24-28 3-D rotation x / y / z /
    // perspective / preset}
    const model::ShapeEffects& f = sh.fx;
    w.Int(30);
    w.Map(28);
    w.KInt(1, f.preset);
    w.KInt(2, f.theme_style);
    w.KInt(3, f.shadow);
    w.KInt(4, f.shadow_color);
    w.KDouble(5, f.shadow_transparency);
    w.KDouble(6, f.shadow_size);
    w.KDouble(7, f.shadow_blur);
    w.KDouble(8, f.shadow_angle);
    w.KDouble(9, f.shadow_distance);
    w.KDouble(10, f.refl_size);
    w.KDouble(11, f.refl_transparency);
    w.KDouble(12, f.refl_blur);
    w.KDouble(13, f.refl_distance);
    w.KDouble(14, f.glow_size);
    w.KInt(15, f.glow_color);
    w.KDouble(16, f.glow_transparency);
    w.KDouble(17, f.soft_edge);
    w.KInt(18, f.bevel_top);
    w.KInt(19, f.bevel_bottom);
    w.KDouble(20, f.bevel_top_w);
    w.KDouble(21, f.bevel_top_h);
    w.KDouble(22, f.bevel_bottom_w);
    w.KDouble(23, f.bevel_bottom_h);
    w.KDouble(24, f.rot_x);
    w.KDouble(25, f.rot_y);
    w.KDouble(26, f.rot_z);
    w.KDouble(27, f.perspective);
    w.KInt(28, f.rot_preset);
  }
  w.Nil();
}

bool ReadShapeExtra(MsgReader& r, model::ShapeObj* sh) {
  if (r.Nil()) return true;
  uint32_t n = 0;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k = 0, iv = 0;
    double dv = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1: ok = r.IntAs(&sh->line_dash); break;
      case 2: ok = r.IntAs(&sh->begin_arrow); break;
      case 3: ok = r.IntAs(&sh->end_arrow); break;
      case 4: ok = r.Double(&dv); sh->fill_transparency = static_cast<float>(dv); break;
      case 5: ok = r.Double(&sh->margin_l); break;
      case 6: ok = r.Double(&sh->margin_t); break;
      case 7: ok = r.Double(&sh->margin_r); break;
      case 8: ok = r.Double(&sh->margin_b); break;
      case 9: ok = r.Bool(&sh->wrap); break;
      case 10: ok = r.Bool(&sh->autofit); break;
      case 11: ok = r.Str(&sh->alt_text); break;
      case 12: ok = r.Double(&sh->crop_l); break;
      case 13: ok = r.Double(&sh->crop_t); break;
      case 14: ok = r.Double(&sh->crop_r); break;
      case 15: ok = r.Double(&sh->crop_b); break;
      case 16: ok = r.Bool(&sh->locked); break;
      case 17: ok = r.Bool(&sh->lock_aspect); break;
      case 18: ok = r.Double(&sh->rotation); break;
      case 19: ok = r.Int(&iv); sh->group = static_cast<uint32_t>(iv); break;
      case 20: ok = r.Bool(&sh->print_object); break;
      case 21: ok = r.Double(&sh->orig_w); break;
      case 22: ok = r.Double(&sh->orig_h); break;
      case 23: ok = r.IntAs(&sh->auto_type); break;
      case 24: ok = r.IntAs(&sh->connector); break;
      case 25: {
        uint32_t m = 0;
        ok = r.Array(&m);
        sh->path.clear();
        for (uint32_t j = 0; ok && j < m; ++j) {
          double v = 0;
          ok = r.Double(&v);
          sh->path.push_back(v);
        }
        break;
      }
      case 26: {
        int f = 0;
        ok = r.IntAs(&f);
        sh->path_closed = (f & 1) != 0;
        sh->path_smooth = (f & 2) != 0;
        break;
      }
      case 27: ok = r.IntAs(&sh->text_outline); break;
      case 28: ok = r.Bool(&sh->text_shadow); break;
      case 30: {  // Format Shape > Effects
        uint32_t m = 0;
        ok = r.Map(&m);
        model::ShapeEffects& f = sh->fx;
        for (uint32_t j = 0; ok && j < m; ++j) {
          int64_t fk = 0;
          double d = 0;
          ok = r.Int(&fk);
          if (!ok) break;
          int* ints[] = {nullptr, &f.preset, &f.theme_style, &f.shadow};
          float* floats[] = {&f.shadow_transparency, &f.shadow_size, &f.shadow_blur, &f.shadow_angle, &f.shadow_distance, &f.refl_size, &f.refl_transparency,
                             &f.refl_blur, &f.refl_distance, &f.glow_size};
          if (fk >= 1 && fk <= 3) ok = r.IntAs(ints[fk]);
          else if (fk == 4) ok = r.IntAs(&f.shadow_color);
          else if (fk >= 5 && fk <= 14) ok = r.Double(&d), *floats[fk - 5] = static_cast<float>(d);
          else if (fk == 15) ok = r.IntAs(&f.glow_color);
          else if (fk == 16) ok = r.Double(&d), f.glow_transparency = static_cast<float>(d);
          else if (fk == 17) ok = r.Double(&d), f.soft_edge = static_cast<float>(d);
          else if (fk == 18) ok = r.IntAs(&f.bevel_top);
          else if (fk == 19) ok = r.IntAs(&f.bevel_bottom);
          else if (fk >= 20 && fk <= 27) {
            float* more[] = {&f.bevel_top_w, &f.bevel_top_h, &f.bevel_bottom_w, &f.bevel_bottom_h, &f.rot_x, &f.rot_y, &f.rot_z, &f.perspective};
            ok = r.Double(&d);
            *more[fk - 20] = static_cast<float>(d);
          } else if (fk == 28) ok = r.IntAs(&f.rot_preset);
          else ok = r.Skip();
        }
        break;
      }

      case 31: ok = r.IntAs(&sh->on_chart); break;
      case 29: {
        uint32_t na = 0;
        ok = r.Array(&na);
        sh->adj.clear();
        for (uint32_t j = 0; ok && j < na; ++j) {
          double v = 0;
          ok = r.Double(&v);
          sh->adj.push_back(v);
        }
        break;
      }
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

void WriteControl(MsgWriter& w, const model::Control& c) {
  w.Map(47);
  w.KBool(43, c.print_object);
  w.KStr(44, c.alt_text);
  w.Int(45);
  w.Array(4);
  w.Int(c.text_halign);
  w.Int(c.text_valign);
  w.Int(c.text_orientation);
  w.Bool(c.auto_size);
  w.Int(46);
  w.Array(5);
  w.Bool(c.auto_margins);
  w.Double(c.margin_l);
  w.Double(c.margin_t);
  w.Double(c.margin_r);
  w.Double(c.margin_b);
  w.KBool(47, c.shading_3d);
  w.KBool(41, c.form);
  w.KStr(42, c.on_action);
  w.KInt(1, c.id);
  w.KInt(2, static_cast<int>(c.kind));
  w.KStr(3, c.name);
  w.KStr(4, c.caption);
  w.KDouble(5, c.left);
  w.KDouble(6, c.top);
  w.KDouble(7, c.width);
  w.KDouble(8, c.height);
  w.KInt(9, static_cast<int>(c.placement));
  w.KBool(10, c.enabled);
  w.KBool(11, c.visible);
  w.KInt(12, c.back_color);
  w.KInt(13, c.fore_color);
  w.KStr(14, c.font_name);
  w.KDouble(15, c.font_size);
  w.KBool(16, c.bold);
  w.Int(17);
  w.Array(static_cast<uint32_t>(c.items.size()));
  for (const auto& s : c.items) w.Str(s);
  w.KStr(18, c.list_fill_range);
  w.KStr(19, c.linked_cell);
  w.KInt(20, c.style);
  w.KStr(21, c.value);
  w.KInt(22, c.list_index);
  w.KInt(23, c.z);
  w.KBool(24, c.created_by_python);
  w.KStr(25, c.group_name);
  w.KInt(26, c.min);
  w.KInt(27, c.max);
  w.KInt(28, c.small_change);
  w.KInt(29, c.large_change);
  w.KInt(30, c.orientation);
  w.KInt(31, c.multi_select);
  w.Int(32);
  w.Array(static_cast<uint32_t>(c.selected.size()));
  for (int i : c.selected) w.Int(i);
  w.KBool(33, c.multi_line);
  w.KStr(34, c.password_char);
  w.Int(35);
  w.Bin(c.picture);
  w.KInt(36, c.picture_size_mode);
  w.KBool(37, c.italic);
  w.KBool(38, c.locked);
  w.KBool(39, c.word_wrap);
  w.KInt(40, c.list_rows);
}

bool ReadControl(MsgReader& r, model::Control* c) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k;
    if (!r.Int(&k)) return false;
    bool ok = true;
    int64_t iv;
    double dv;
    switch (k) {
      case 1: ok = r.IntAs(&c->id); break;
      case 2: ok = r.Int(&iv); c->kind = static_cast<model::ControlKind>(iv); break;
      case 3: ok = r.Str(&c->name); break;
      case 4: ok = r.Str(&c->caption); break;
      case 5: ok = r.Double(&c->left); break;
      case 6: ok = r.Double(&c->top); break;
      case 7: ok = r.Double(&c->width); break;
      case 8: ok = r.Double(&c->height); break;
      case 9: ok = r.Int(&iv); c->placement = static_cast<model::Placement>(iv); break;
      case 10: ok = r.Bool(&c->enabled); break;
      case 11: ok = r.Bool(&c->visible); break;
      case 12: ok = r.IntAs(&c->back_color); break;
      case 13: ok = r.IntAs(&c->fore_color); break;
      case 14: ok = r.Str(&c->font_name); break;
      case 15: ok = r.Double(&dv); c->font_size = static_cast<float>(dv); break;
      case 16: ok = r.Bool(&c->bold); break;
      case 17: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          std::string s;
          ok = r.Str(&s);
          c->items.push_back(s);
        }
        break;
      }
      case 18: ok = r.Str(&c->list_fill_range); break;
      case 19: ok = r.Str(&c->linked_cell); break;
      case 20: ok = r.IntAs(&c->style); break;
      case 21: ok = r.Str(&c->value); break;
      case 22: ok = r.IntAs(&c->list_index); break;
      case 23: ok = r.IntAs(&c->z); break;
      case 24: ok = r.Bool(&c->created_by_python); break;
      case 25: ok = r.Str(&c->group_name); break;
      case 26: ok = r.IntAs(&c->min); break;
      case 27: ok = r.IntAs(&c->max); break;
      case 28: ok = r.IntAs(&c->small_change); break;
      case 29: ok = r.IntAs(&c->large_change); break;
      case 30: ok = r.IntAs(&c->orientation); break;
      case 31: ok = r.IntAs(&c->multi_select); break;
      case 32: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          int v = 0;
          ok = r.IntAs(&v);
          c->selected.push_back(v);
        }
        break;
      }
      case 33: ok = r.Bool(&c->multi_line); break;
      case 34: ok = r.Str(&c->password_char); break;
      case 35: ok = r.Bin(&c->picture); break;
      case 36: ok = r.IntAs(&c->picture_size_mode); break;
      case 37: ok = r.Bool(&c->italic); break;
      case 38: ok = r.Bool(&c->locked); break;
      case 39: ok = r.Bool(&c->word_wrap); break;
      case 40: ok = r.IntAs(&c->list_rows); break;
      case 41: ok = r.Bool(&c->form); break;
      case 42: ok = r.Str(&c->on_action); break;
      case 43: ok = r.Bool(&c->print_object); break;
      case 44: ok = r.Str(&c->alt_text); break;
      case 45: {
        uint32_t na = 0;
        ok = r.Array(&na) && na >= 4 && r.IntAs(&c->text_halign) && r.IntAs(&c->text_valign) && r.IntAs(&c->text_orientation) && r.Bool(&c->auto_size);
        for (uint32_t z = 4; ok && z < na; ++z) ok = r.Skip();
        break;
      }
      case 46: {
        uint32_t na = 0;
        ok = r.Array(&na) && na >= 5 && r.Bool(&c->auto_margins) && r.Double(&c->margin_l) && r.Double(&c->margin_t) && r.Double(&c->margin_r) && r.Double(&c->margin_b);
        for (uint32_t z = 5; ok && z < na; ++z) ok = r.Skip();
        break;
      }
      case 47: ok = r.Bool(&c->shading_3d); break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

// a chart text's font: [size, color, bold], then (an older reader skips them) [name, italic, underline]
void WriteChartFont(MsgWriter& w, const model::ChartFont& f) {
  w.Array(6);
  w.Double(f.size);
  w.Int(f.color);
  w.Bool(f.bold);
  w.Str(f.name);
  w.Bool(f.italic);
  w.Bool(f.underline);
}

bool ReadChartFont(MsgReader& r, model::ChartFont* f) {
  uint32_t n = 0;
  double size = 0;
  if (!r.Array(&n) || n < 3 || !r.Double(&size) || !r.IntAs(&f->color) || !r.Bool(&f->bold)) return false;
  f->size = static_cast<float>(size);
  for (uint32_t i = 3; i < n; ++i) {
    bool ok = true;
    switch (i) {
      case 3: ok = r.Str(&f->name); break;
      case 4: ok = r.Bool(&f->italic); break;
      case 5: ok = r.Bool(&f->underline); break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

// an axis: [visible, has title, title, number format, gridlines], then Format Axis' options (an older reader skips
// them): [min auto, max auto, min, max, major, minor, log, log base, reverse, crosses, crosses at, display unit,
// major tick, minor tick, label position, minor gridlines, between, label interval], then [own font, font, title
// font, line color, no line, line weight, label distance]
void WriteAxisFormat(MsgWriter& w, const model::ChartAxisFormat& a) {
  w.Array(33);
  w.Bool(a.visible);
  w.Bool(a.has_title);
  w.Str(a.title);
  w.Str(a.number_format);
  w.Bool(a.gridlines);
  w.Bool(a.min_auto);
  w.Bool(a.max_auto);
  w.Double(a.min);
  w.Double(a.max);
  w.Double(a.major);
  w.Double(a.minor);
  w.Bool(a.log);
  w.Double(a.log_base);
  w.Bool(a.reverse);
  w.Int(a.crosses);
  w.Double(a.crosses_at);
  w.Double(a.display_unit);
  w.Int(a.major_tick);
  w.Int(a.minor_tick);
  w.Int(a.label_pos);
  w.Bool(a.minor_gridlines);
  w.Bool(a.between);
  w.Int(a.label_interval);
  w.Bool(a.own_font);
  WriteChartFont(w, a.font);
  WriteChartFont(w, a.title_font);
  w.Int(a.line_color);
  w.Bool(a.no_line);
  w.Double(a.line_weight);
  w.Int(a.label_offset);
  w.Bool(a.multi_level);  // 30: Multi-level Category Labels
  w.Int(a.mark_interval);  // 31: Axis.TickMarkSpacing
  w.Bool(a.unit_label);    // 32: the display unit's label
}

// *ext: the axis had Format Axis' options (an older file keeps its bounds in the chart's keys 13-15)
bool ReadAxisFormat(MsgReader& r, model::ChartAxisFormat* a, bool* ext = nullptr) {
  uint32_t n = 0;
  if (!r.Array(&n) || n < 5 || !r.Bool(&a->visible) || !r.Bool(&a->has_title) || !r.Str(&a->title) || !r.Str(&a->number_format) || !r.Bool(&a->gridlines))
    return false;
  if (ext) *ext = n >= 9;
  for (uint32_t i = 5; i < n; ++i) {
    bool ok = true;
    switch (i) {
      case 5: ok = r.Bool(&a->min_auto); break;
      case 6: ok = r.Bool(&a->max_auto); break;
      case 7: ok = r.Double(&a->min); break;
      case 8: ok = r.Double(&a->max); break;
      case 9: ok = r.Double(&a->major); break;
      case 10: ok = r.Double(&a->minor); break;
      case 11: ok = r.Bool(&a->log); break;
      case 12: ok = r.Double(&a->log_base); break;
      case 13: ok = r.Bool(&a->reverse); break;
      case 14: ok = r.IntAs(&a->crosses); break;
      case 15: ok = r.Double(&a->crosses_at); break;
      case 16: ok = r.Double(&a->display_unit); break;
      case 17: ok = r.IntAs(&a->major_tick); break;
      case 18: ok = r.IntAs(&a->minor_tick); break;
      case 19: ok = r.IntAs(&a->label_pos); break;
      case 20: ok = r.Bool(&a->minor_gridlines); break;
      case 21: ok = r.Bool(&a->between); break;
      case 22: ok = r.IntAs(&a->label_interval); break;
      case 23: ok = r.Bool(&a->own_font); break;
      case 24: ok = ReadChartFont(r, &a->font); break;
      case 25: ok = ReadChartFont(r, &a->title_font); break;
      case 26: ok = r.IntAs(&a->line_color); break;
      case 27: ok = r.Bool(&a->no_line); break;
      case 28: {
        double lw = 0;
        ok = r.Double(&lw);
        a->line_weight = static_cast<float>(lw);
        break;
      }
      case 29: ok = r.IntAs(&a->label_offset); break;
      case 30: ok = r.Bool(&a->multi_level); break;
      case 31: ok = r.IntAs(&a->mark_interval); break;
      case 32: ok = r.Bool(&a->unit_label); break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

void WriteManualRect(MsgWriter& w, const model::ChartObj::ManualRect& m) {
  w.Array(5);
  w.Bool(m.on);
  w.Double(m.x);
  w.Double(m.y);
  w.Double(m.w);
  w.Double(m.h);
}

bool ReadManualRect(MsgReader& r, model::ChartObj::ManualRect* m) {
  uint32_t n = 0;
  double x = 0, y = 0, ww = 0, hh = 0;
  if (!r.Array(&n) || n < 5 || !r.Bool(&m->on) || !r.Double(&x) || !r.Double(&y) || !r.Double(&ww) || !r.Double(&hh)) return false;
  m->x = static_cast<float>(x);
  m->y = static_cast<float>(y);
  m->w = static_cast<float>(ww);
  m->h = static_cast<float>(hh);
  for (uint32_t i = 5; i < n; ++i) {
    if (!r.Skip()) return false;
  }
  return true;
}

// ---- chart keys 50-55 and series keys 56-58: chart types' options, stock and 3-D elements, date axis, properties ----
constexpr uint32_t kChartTypeKeys = 6;   // chart keys 50-55
constexpr uint32_t kSeriesTypeKeys = 3;  // series keys 56-58

void WriteIntList(MsgWriter& w, const std::vector<int>& v) {
  w.Array(static_cast<uint32_t>(v.size()));
  for (int k : v) w.Int(k);
}
bool ReadIntList(MsgReader& r, std::vector<int>* v) {
  uint32_t n = 0;
  if (!r.Array(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k = 0;
    if (!r.Int(&k)) return false;
    if (k >= 0 && k < 1000000) v->push_back(static_cast<int>(k));
  }
  return true;
}

void WriteChartTypeKeys(MsgWriter& w, const model::ChartObj& ch) {
  // 50: pie of pie / bar of pie {1 split type, 2 split value, 3 second plot size, 4 gap, 5 custom points}
  w.Int(50);
  w.Map(5);
  w.KInt(1, ch.split_type);
  w.KDouble(2, ch.split_value);
  w.KInt(3, ch.second_size);
  w.KInt(4, ch.pie_gap);
  w.Int(5);
  WriteIntList(w, ch.second_points);
  // 51: stock {1 up / down bars, 2 high-low lines, 3 up fill, 4 down fill, 5 high-low line color, 6 gap width}
  w.Int(51);
  w.Map(6);
  w.KBool(1, ch.up_down_bars);
  w.KBool(2, ch.hi_lo_lines);
  w.KInt(3, ch.up_fill);
  w.KInt(4, ch.down_fill);
  w.KInt(5, ch.hilo_line);
  w.KInt(6, ch.updown_gap);
  // 52: 3-D {1 floor fill, 2 walls fill}
  w.Int(52);
  w.Map(2);
  w.KInt(1, ch.floor_fill);
  w.KInt(2, ch.wall_fill);
  // 53: date axis {1 category type, 2 base unit, 3 major unit scale}
  w.Int(53);
  w.Map(3);
  w.KInt(1, ch.cat_type);
  w.KInt(2, ch.base_unit);
  w.KInt(3, ch.major_unit_scale);
  // 54: the axis titles' manual layout {1 category, 2 value, 3 secondary value}
  w.Int(54);
  w.Map(3);
  w.Int(1);
  WriteManualRect(w, ch.cat_title_rect);
  w.Int(2);
  WriteManualRect(w, ch.val_title_rect);
  w.Int(3);
  WriteManualRect(w, ch.val2_title_rect);
  // 55: properties {1 print object, 2 locked}
  w.Int(55);
  w.Map(2);
  w.KBool(1, ch.print_object);
  w.KBool(2, ch.locked);
}

bool ReadChartTypeKey(MsgReader& r, int64_t key, model::ChartObj* ch) {
  uint32_t m = 0;
  if (!r.Map(&m)) return false;
  for (uint32_t j = 0; j < m; ++j) {
    int64_t k = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (key * 100 + k) {
      case 5001: ok = r.IntAs(&ch->split_type); break;
      case 5002: ok = r.Double(&ch->split_value); break;
      case 5003: ok = r.IntAs(&ch->second_size); break;
      case 5004: ok = r.IntAs(&ch->pie_gap); break;
      case 5005: ok = ReadIntList(r, &ch->second_points); break;
      case 5101: ok = r.Bool(&ch->up_down_bars); break;
      case 5102: ok = r.Bool(&ch->hi_lo_lines); break;
      case 5103: ok = r.IntAs(&ch->up_fill); break;
      case 5104: ok = r.IntAs(&ch->down_fill); break;
      case 5105: ok = r.IntAs(&ch->hilo_line); break;
      case 5106: ok = r.IntAs(&ch->updown_gap); break;
      case 5201: ok = r.IntAs(&ch->floor_fill); break;
      case 5202: ok = r.IntAs(&ch->wall_fill); break;
      case 5301: ok = r.IntAs(&ch->cat_type); break;
      case 5302: ok = r.IntAs(&ch->base_unit); break;
      case 5303: ok = r.IntAs(&ch->major_unit_scale); break;
      case 5401: ok = ReadManualRect(r, &ch->cat_title_rect); break;
      case 5402: ok = ReadManualRect(r, &ch->val_title_rect); break;
      case 5403: ok = ReadManualRect(r, &ch->val2_title_rect); break;
      case 5501: ok = r.Bool(&ch->print_object); break;
      case 5502: ok = r.Bool(&ch->locked); break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

void WriteTrendline(MsgWriter& w, const model::ChartTrendline& t) {
  w.Array(10);
  w.Int(static_cast<int>(t.kind));
  w.Int(t.order);
  w.Int(t.period);
  w.Double(t.forward);
  w.Double(t.backward);
  w.Bool(t.eq);
  w.Bool(t.r2);
  w.Str(t.name);
  w.Bool(t.intercept_on);
  w.Double(t.intercept);
}

void WriteSeriesTypeKeys(MsgWriter& w, const model::ChartSeries& s) {
  // 56: type options {1 treemap labels, 2 inclusive median, 3-5 map projection / area / labels, 6 map colors kind,
  // 7-9 map colors, 10 error bar end cap, 11-12 data labels' offset}
  w.Int(56);
  w.Map(12);
  w.KInt(1, s.parent_labels);
  w.KBool(2, s.inclusive_median);
  w.KInt(3, s.geo_projection);
  w.KInt(4, s.geo_area);
  w.KInt(5, s.geo_labels);
  w.KInt(6, s.geo_colors);
  w.KInt(7, s.geo_min);
  w.KInt(8, s.geo_mid);
  w.KInt(9, s.geo_max);
  w.KBool(10, s.err_cap);
  w.KDouble(11, s.label_dx);
  w.KDouble(12, s.label_dy);
  // 57: trendlines {1 name, 2 set intercept, 3 intercept, 4 more trendlines
  // [kind, order, period, forward, backward, equation, R-squared, name, set intercept, intercept]}
  w.Int(57);
  w.Map(4);
  w.KStr(1, s.trend_name);
  w.KBool(2, s.trend_icept_on);
  w.KDouble(3, s.trend_icept);
  w.Int(4);
  w.Array(static_cast<uint32_t>(s.more_trends.size()));
  for (const auto& t : s.more_trends) WriteTrendline(w, t);
  // 58: X error bars [kind, amount, plus, minus, include]
  w.Int(58);
  w.Array(5);
  w.Int(static_cast<int>(s.xerr));
  w.Double(s.xerr_amount);
  w.Str(s.xerr_plus);
  w.Str(s.xerr_minus);
  w.Int(s.xerr_include);
}

bool ReadSeriesTypeKey(MsgReader& r, int64_t key, model::ChartSeries* s) {
  if (key == 58) {
    uint32_t n = 0;
    int64_t kind = 0;
    if (!r.Array(&n) || n < 5 || !r.Int(&kind) || !r.Double(&s->xerr_amount) || !r.Str(&s->xerr_plus) || !r.Str(&s->xerr_minus) || !r.IntAs(&s->xerr_include)) return false;
    s->xerr = static_cast<model::ErrorBarKind>(std::clamp<int64_t>(kind, 0, 5));
    for (uint32_t i = 5; i < n; ++i) {
      if (!r.Skip()) return false;
    }
    return true;
  }
  uint32_t m = 0;
  if (!r.Map(&m)) return false;
  for (uint32_t j = 0; j < m; ++j) {
    int64_t k = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    double dv = 0;
    switch (key * 100 + k) {
      case 5601: ok = r.IntAs(&s->parent_labels); break;
      case 5602: ok = r.Bool(&s->inclusive_median); break;
      case 5603: ok = r.IntAs(&s->geo_projection); break;
      case 5604: ok = r.IntAs(&s->geo_area); break;
      case 5605: ok = r.IntAs(&s->geo_labels); break;
      case 5606: ok = r.IntAs(&s->geo_colors); break;
      case 5607: ok = r.IntAs(&s->geo_min); break;
      case 5608: ok = r.IntAs(&s->geo_mid); break;
      case 5609: ok = r.IntAs(&s->geo_max); break;
      case 5610: ok = r.Bool(&s->err_cap); break;
      case 5611: ok = r.Double(&dv), s->label_dx = static_cast<float>(dv); break;
      case 5612: ok = r.Double(&dv), s->label_dy = static_cast<float>(dv); break;
      case 5701: ok = r.Str(&s->trend_name); break;
      case 5702: ok = r.Bool(&s->trend_icept_on); break;
      case 5703: ok = r.Double(&s->trend_icept); break;
      case 5704: {
        uint32_t n = 0;
        ok = r.Array(&n);
        for (uint32_t i = 0; ok && i < n; ++i) {
          uint32_t f = 0;
          int64_t kind = 1;
          model::ChartTrendline t;
          ok = r.Array(&f) && f >= 10 && r.Int(&kind) && r.IntAs(&t.order) && r.IntAs(&t.period) && r.Double(&t.forward) && r.Double(&t.backward) && r.Bool(&t.eq) &&
               r.Bool(&t.r2) && r.Str(&t.name) && r.Bool(&t.intercept_on) && r.Double(&t.intercept);
          for (uint32_t e = 10; ok && e < f; ++e) ok = r.Skip();
          t.kind = static_cast<model::TrendKind>(std::clamp<int64_t>(kind, 1, 6));
          if (ok) s->more_trends.push_back(std::move(t));
        }
        break;
      }
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

void WriteChart(MsgWriter& w, const model::ChartObj& ch) {
  w.Map(41 + kChartTypeKeys /* 50-55 */);
  WriteChartTypeKeys(w, ch);
  // 33-41: hidden and empty cells, overlay title, style, colors, histogram bins, waterfall connectors, aspect lock
  w.KBool(33, ch.plot_visible_only);
  w.KInt(34, ch.display_blanks);
  w.KBool(35, ch.na_as_blank);
  w.KBool(36, ch.title_overlay);
  w.KInt(37, ch.style);
  w.KInt(38, ch.color_scheme);
  w.Int(39);
  w.Array(5);
  w.Bool(ch.bins_overflow);
  w.Double(ch.overflow_at);
  w.Bool(ch.bins_underflow);
  w.Double(ch.underflow_at);
  w.Bool(ch.bins_by_category);
  w.KBool(40, ch.connectors);
  w.KBool(41, ch.lock_aspect);
  // 29-32: gap width, overlap, angle of the first slice, doughnut hole size
  w.KInt(29, ch.gap_width);
  w.KInt(30, ch.overlap);
  w.KInt(31, ch.first_slice_angle);
  w.KInt(32, ch.hole_size);
  // 28: Chart Filters' hidden categories (0-based points)
  w.Int(28);
  w.Array(static_cast<uint32_t>(ch.hidden_cats.size()));
  for (int k : ch.hidden_cats) w.Int(k);
  // 27: 3-D view [set, rotation, elevation, perspective, depth percent]
  w.Int(27);
  w.Array(5);
  w.Bool(ch.view3d);
  w.Double(ch.rotation);
  w.Double(ch.elevation);
  w.Double(ch.perspective);
  w.Double(ch.depth_percent);
  // 26: element formatting {1 title font, 2 legend font, 3 axis font, 4 legend position, 5 chart fill, 6 chart border,
  // 7 no border, 8 plot fill, 9 category axis, 10 value axis, 11 secondary value axis, 12 plot border, 13 legend fill,
  // 14 legend border, 15 title fill, 16 title border, 17-19 the title's, legend's and plot area's manual layout,
  // 20 legend overlay}; a font is [size, color, bold], an axis [visible, has title, title, number format, gridlines, ...]
  w.Int(26);
  w.Map(20);
  w.Int(1);
  WriteChartFont(w, ch.title_font);
  w.Int(2);
  WriteChartFont(w, ch.legend_font);
  w.Int(3);
  WriteChartFont(w, ch.axis_font);
  w.KInt(4, ch.legend_pos);
  w.KInt(5, ch.chart_fill);
  w.KInt(6, ch.chart_line);
  w.KBool(7, ch.chart_line_none);
  w.KInt(8, ch.plot_fill);
  w.Int(9);
  WriteAxisFormat(w, ch.cat_axis);
  w.Int(10);
  WriteAxisFormat(w, ch.val_axis);
  w.Int(11);
  WriteAxisFormat(w, ch.val_axis2);
  w.KInt(12, ch.plot_line);
  w.KInt(13, ch.legend_fill);
  w.KInt(14, ch.legend_line);
  w.KInt(15, ch.title_fill);
  w.KInt(16, ch.title_line);
  w.Int(17);
  WriteManualRect(w, ch.title_rect);
  w.Int(18);
  WriteManualRect(w, ch.legend_rect);
  w.Int(19);
  WriteManualRect(w, ch.plot_rect);
  w.KBool(20, ch.legend_overlay);
  w.KDouble(22, ch.bin_width);
  w.KInt(23, ch.bin_count);
  w.KStr(24, ch.source);
  w.KBool(25, ch.source_by_rows);
  w.KStr(21, ch.pivot);
  w.KInt(1, ch.id);
  w.KStr(2, ch.name);
  w.KInt(3, static_cast<int>(ch.type));
  w.KDouble(4, ch.left);
  w.KDouble(5, ch.top);
  w.KDouble(6, ch.width);
  w.KDouble(7, ch.height);
  w.KInt(8, static_cast<int>(ch.placement));
  w.KStr(9, ch.title);
  w.KBool(10, ch.has_title);
  w.KBool(11, ch.has_legend);
  w.KBool(12, ch.interactive);
  // 13-15: the bounds as older versions keep them (now in the axes)
  w.KBool(13, ch.val_axis.min_auto && ch.val_axis.max_auto);
  w.KBool(14, ch.cat_axis.min_auto && ch.cat_axis.max_auto);
  w.Int(15);
  w.Array(4);
  w.Double(ch.val_axis.min);
  w.Double(ch.val_axis.max);
  w.Double(ch.cat_axis.min);
  w.Double(ch.cat_axis.max);
  w.Int(16);
  w.Array(static_cast<uint32_t>(ch.series.size()));
  for (const auto& s : ch.series) {
    // 8-10: series data supplied by Python (Series.SetData / AppendData), little-endian f64
    const bool data = s.python_data && s.data;
    w.Map((data ? 48 : 46) + 7 /* 49-55 */ + kSeriesTypeKeys /* 56-58 */);
    WriteSeriesTypeKeys(w, s);
    // 49-53: Fill & Line (fill type, gradient, pattern, picture; compound, cap, join, arrows), label font, flags
    w.Int(49);
    w.Array(12);
    w.Int(s.fill_type);
    w.Int(s.grad_color2);
    w.Int(s.grad_angle);
    w.Int(s.pattern);
    w.Int(s.pattern_back);
    w.Bool(s.picture_tile);
    w.Double(s.tile_ox);
    w.Double(s.tile_oy);
    w.Double(s.tile_sx);
    w.Double(s.tile_sy);
    w.Int(s.tile_align);
    w.Int(s.tile_mirror);
    w.Int(50);
    w.Bin(s.picture);
    w.Int(51);
    w.Array(5);
    w.Int(s.compound);
    w.Int(s.cap);
    w.Int(s.join);
    w.Int(s.begin_arrow);
    w.Int(s.end_arrow);
    w.Int(52);
    WriteChartFont(w, s.label_font);
    w.Int(53);
    w.Array(3);
    w.Bool(s.leader_lines);
    w.Bool(s.vary_points);
    w.Bool(s.legend_hidden);
    // 54: its legend entry's own font [set, font]
    w.Int(54);
    w.Array(2);
    w.Bool(s.legend_entry_font_set);
    WriteChartFont(w, s.legend_entry_font);
    // 55: its data labels' [fill, border]
    w.Int(55);
    w.Array(2);
    w.Int(s.label_fill);
    w.Int(s.label_line);
    // 32: points with their own format [index, fill, border, explosion, label]
    w.Int(32);
    w.Array(static_cast<uint32_t>(s.points.size()));
    for (const auto& p : s.points) {
      // then (an older reader skips them) its data label's offset [dx, dy], own text, contents, position, [font set,
      // font], its category legend entry's [font set, font], its label's [fill, border], its legend entry deleted
      w.Array(17);
      w.Int(p.index);
      w.Int(p.fill);
      w.Int(p.line);
      w.Int(p.explosion);
      w.Int(p.label);
      w.Double(p.label_dx);
      w.Double(p.label_dy);
      w.Str(p.label_text);
      w.Int(p.label_show);
      w.Int(p.label_pos);
      w.Bool(p.label_font_set);
      WriteChartFont(w, p.label_font);
      w.Bool(p.legend_font_set);
      WriteChartFont(w, p.legend_font);
      w.Int(p.label_fill);
      w.Int(p.label_line);
      w.Bool(p.legend_hidden);
    }
    // 33-40: pie explosion, vary colors by slice, data labels (position, contents, cells, number format, separator),
    // bubble sizes
    w.KInt(33, s.explosion);
    w.KBool(34, s.vary_colors);
    w.KInt(35, s.label_pos);
    w.KInt(36, s.label_show);
    w.KStr(37, s.label_range);
    w.KStr(38, s.label_format);
    w.KStr(39, s.label_sep);
    w.KStr(40, s.bubble_sizes);
    // 41-48: Fill & Line
    w.KInt(41, s.line_color);
    w.KBool(42, s.no_fill);
    w.KBool(43, s.no_line);
    w.KInt(44, s.dash);
    w.KInt(45, s.transparency);
    w.KBool(46, s.invert_negative);
    w.KInt(47, s.invert_color);
    w.Int(48);
    w.Array(2);
    w.Int(s.marker_fill);
    w.Int(s.marker_line);
    // 30-31: smoothed line, box and whisker options
    w.KBool(30, s.smooth);
    w.KInt(31, s.box_flags);
    // 29: left out by Chart Filters
    w.KBool(29, s.filtered);
    // 26-28: custom error amounts (plus, minus) and which directions are drawn
    w.KStr(26, s.err_plus);
    w.KStr(27, s.err_minus);
    w.KInt(28, s.err_include);
    // 23-25: line weight, marker, marker size
    w.KDouble(23, s.line_weight);
    w.KInt(24, s.marker);
    w.KDouble(25, s.marker_size);
    w.KStr(1, s.name);
    w.KStr(2, s.values);
    w.KStr(3, s.x_values);
    w.KInt(4, s.axis_group);
    w.KInt(5, s.color);
    w.KStr(6, s.topic);
    w.KInt(7, s.capacity);
    w.KBool(8, s.python_data);
    // 11-22: combo type, data labels, trendline, error bars, waterfall totals
    w.KInt(11, s.type);
    w.KBool(12, s.data_labels);
    w.KInt(13, static_cast<int>(s.trend));
    w.KInt(14, s.trend_order);
    w.KInt(15, s.trend_period);
    w.KDouble(16, s.trend_forward);
    w.KDouble(17, s.trend_backward);
    w.KBool(18, s.trend_eq);
    w.KBool(19, s.trend_r2);
    w.KInt(20, static_cast<int>(s.err));
    w.KDouble(21, s.err_amount);
    w.Int(22);
    w.Array(static_cast<uint32_t>(s.totals.size()));
    for (int t : s.totals) w.Int(t);
    if (data) {
      w.Int(9);
      w.Bin(std::string_view(reinterpret_cast<const char*>(s.data->x.data()), s.data->x.size() * sizeof(double)));
      w.Int(10);
      w.Bin(std::string_view(reinterpret_cast<const char*>(s.data->y.data()), s.data->y.size() * sizeof(double)));
    }
  }
  w.KInt(17, ch.z);
  w.KBool(18, ch.created_by_python);
  w.KBool(19, ch.floating);
  w.KInt(20, 1);  // reserved: whether series data is stored separately (P2)
}

bool ReadChart(MsgReader& r, model::ChartObj* ch) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  // an older file keeps the bounds in keys 13-15; a newer one in its axes (read whichever order the keys come in)
  bool y_auto = true, x_auto = true, axes_ext = false;
  double y_min = 0, y_max = 1, x_min = 0, x_max = 1;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k, iv;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1: ok = r.IntAs(&ch->id); break;
      case 2: ok = r.Str(&ch->name); break;
      case 3: ok = r.Int(&iv); ch->type = static_cast<model::ChartType>(std::clamp<int64_t>(iv, 0, model::kChartTypeCount - 1)); break;
      case 21: ok = r.Str(&ch->pivot); break;
      case 4: ok = r.Double(&ch->left); break;
      case 5: ok = r.Double(&ch->top); break;
      case 6: ok = r.Double(&ch->width); break;
      case 7: ok = r.Double(&ch->height); break;
      case 8: ok = r.Int(&iv); ch->placement = static_cast<model::Placement>(iv); break;
      case 9: ok = r.Str(&ch->title); break;
      case 10: ok = r.Bool(&ch->has_title); break;
      case 11: ok = r.Bool(&ch->has_legend); break;
      case 12: ok = r.Bool(&ch->interactive); break;
      case 13: ok = r.Bool(&y_auto); break;
      case 14: ok = r.Bool(&x_auto); break;
      case 15: {
        uint32_t m;
        ok = r.Array(&m) && m >= 4 && r.Double(&y_min) && r.Double(&y_max) && r.Double(&x_min) && r.Double(&x_max);
        for (uint32_t j = 4; ok && j < m; ++j) ok = r.Skip();
        break;
      }
      case 16: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          model::ChartSeries s;
          std::shared_ptr<model::SeriesData> py;
          uint32_t f;
          ok = r.Map(&f);
          for (uint32_t q = 0; ok && q < f; ++q) {
            int64_t sk;
            ok = r.Int(&sk);
            if (!ok) break;
            switch (sk) {
              case 1: ok = r.Str(&s.name); break;
              case 2: ok = r.Str(&s.values); break;
              case 3: ok = r.Str(&s.x_values); break;
              case 4: ok = r.IntAs(&s.axis_group); break;
              case 5: ok = r.IntAs(&s.color); break;
              case 6: ok = r.Str(&s.topic); break;
              case 7: ok = r.IntAs(&s.capacity); break;
              case 8: ok = r.Bool(&s.python_data); break;
              case 11: ok = r.IntAs(&s.type); break;
              case 12: ok = r.Bool(&s.data_labels); break;
              case 13: ok = r.Int(&iv); s.trend = static_cast<model::TrendKind>(std::clamp<int64_t>(iv, 0, 6)); break;
              case 14: ok = r.IntAs(&s.trend_order); break;
              case 15: ok = r.IntAs(&s.trend_period); break;
              case 16: ok = r.Double(&s.trend_forward); break;
              case 17: ok = r.Double(&s.trend_backward); break;
              case 18: ok = r.Bool(&s.trend_eq); break;
              case 19: ok = r.Bool(&s.trend_r2); break;
              case 20: ok = r.Int(&iv); s.err = static_cast<model::ErrorBarKind>(std::clamp<int64_t>(iv, 0, 5)); break;
              case 26: ok = r.Str(&s.err_plus); break;
              case 27: ok = r.Str(&s.err_minus); break;
              case 28: ok = r.IntAs(&s.err_include); break;
              case 29: ok = r.Bool(&s.filtered); break;
              case 30: ok = r.Bool(&s.smooth); break;
              case 31: ok = r.IntAs(&s.box_flags); break;
              case 32: {
                uint32_t pn = 0;
                ok = r.Array(&pn);
                for (uint32_t t = 0; ok && t < pn; ++t) {
                  uint32_t pf = 0;
                  model::ChartPointFormat pt;
                  ok = r.Array(&pf) && pf >= 5 && r.IntAs(&pt.index) && r.IntAs(&pt.fill) && r.IntAs(&pt.line) && r.IntAs(&pt.explosion) && r.IntAs(&pt.label);
                  for (uint32_t e = 5; ok && e < pf; ++e) {
                    double dv = 0;
                    switch (e) {
                      case 5:
                      case 6:
                        ok = r.Double(&dv);
                        (e == 5 ? pt.label_dx : pt.label_dy) = static_cast<float>(dv);
                        break;
                      case 7: ok = r.Str(&pt.label_text); break;
                      case 8: ok = r.IntAs(&pt.label_show); break;
                      case 9: ok = r.IntAs(&pt.label_pos); break;
                      case 10: ok = r.Bool(&pt.label_font_set); break;
                      case 11: ok = ReadChartFont(r, &pt.label_font); break;
                      case 12: ok = r.Bool(&pt.legend_font_set); break;
                      case 13: ok = ReadChartFont(r, &pt.legend_font); break;
                      case 14: ok = r.IntAs(&pt.label_fill); break;
                      case 15: ok = r.IntAs(&pt.label_line); break;
                      case 16: ok = r.Bool(&pt.legend_hidden); break;
                      default: ok = r.Skip(); break;
                    }
                  }
                  if (ok && pt.index >= 0) s.points.push_back(pt);
                }
                break;
              }
              case 33: ok = r.IntAs(&s.explosion); break;
              case 34: ok = r.Bool(&s.vary_colors); break;
              case 35: ok = r.IntAs(&s.label_pos); break;
              case 36: ok = r.IntAs(&s.label_show); break;
              case 37: ok = r.Str(&s.label_range); break;
              case 38: ok = r.Str(&s.label_format); break;
              case 39: ok = r.Str(&s.label_sep); break;
              case 40: ok = r.Str(&s.bubble_sizes); break;
              case 41: ok = r.IntAs(&s.line_color); break;
              case 42: ok = r.Bool(&s.no_fill); break;
              case 43: ok = r.Bool(&s.no_line); break;
              case 44: ok = r.IntAs(&s.dash); break;
              case 45: ok = r.IntAs(&s.transparency); break;
              case 46: ok = r.Bool(&s.invert_negative); break;
              case 47: ok = r.IntAs(&s.invert_color); break;
              case 56:
              case 57:
              case 58: ok = ReadSeriesTypeKey(r, sk, &s); break;
              case 48: {
                uint32_t mn = 0;
                ok = r.Array(&mn) && mn >= 2 && r.IntAs(&s.marker_fill) && r.IntAs(&s.marker_line);
                for (uint32_t e = 2; ok && e < mn; ++e) ok = r.Skip();
                break;
              }
              case 49:
              case 51:
              case 53: {
                uint32_t an = 0;
                ok = r.Array(&an);
                for (uint32_t e = 0; ok && e < an; ++e) {
                  if (sk == 49) {
                    switch (e) {
                      case 0: ok = r.IntAs(&s.fill_type); break;
                      case 1: ok = r.IntAs(&s.grad_color2); break;
                      case 2: ok = r.IntAs(&s.grad_angle); break;
                      case 3: ok = r.IntAs(&s.pattern); break;
                      case 4: ok = r.IntAs(&s.pattern_back); break;
                      case 5: ok = r.Bool(&s.picture_tile); break;
                      case 6:
                      case 7:
                      case 8:
                      case 9: {
                        double tv = 0;
                        ok = r.Double(&tv);
                        (e == 6 ? s.tile_ox : e == 7 ? s.tile_oy : e == 8 ? s.tile_sx : s.tile_sy) = static_cast<float>(tv);
                        break;
                      }
                      case 10: ok = r.IntAs(&s.tile_align); break;
                      case 11: ok = r.IntAs(&s.tile_mirror); break;
                      default: ok = r.Skip(); break;
                    }
                  } else if (sk == 51) {
                    switch (e) {
                      case 0: ok = r.IntAs(&s.compound); break;
                      case 1: ok = r.IntAs(&s.cap); break;
                      case 2: ok = r.IntAs(&s.join); break;
                      case 3: ok = r.IntAs(&s.begin_arrow); break;
                      case 4: ok = r.IntAs(&s.end_arrow); break;
                      default: ok = r.Skip(); break;
                    }
                  } else {
                    switch (e) {
                      case 0: ok = r.Bool(&s.leader_lines); break;
                      case 1: ok = r.Bool(&s.vary_points); break;
                      case 2: ok = r.Bool(&s.legend_hidden); break;
                      default: ok = r.Skip(); break;
                    }
                  }
                }
                break;
              }
              case 50: ok = r.Bin(&s.picture); break;
              case 52: ok = ReadChartFont(r, &s.label_font); break;
              case 55: {
                uint32_t ln = 0;
                ok = r.Array(&ln) && ln >= 2 && r.IntAs(&s.label_fill) && r.IntAs(&s.label_line);
                for (uint32_t e = 2; ok && e < ln; ++e) ok = r.Skip();
                break;
              }
              case 54: {
                uint32_t fn = 0;
                ok = r.Array(&fn) && fn >= 2 && r.Bool(&s.legend_entry_font_set) && ReadChartFont(r, &s.legend_entry_font);
                for (uint32_t e = 2; ok && e < fn; ++e) ok = r.Skip();
                break;
              }
              case 21: ok = r.Double(&s.err_amount); break;
              case 23: {
                double v = 0;
                ok = r.Double(&v);
                s.line_weight = static_cast<float>(v);
                break;
              }
              case 24: ok = r.IntAs(&s.marker); break;
              case 25: {
                double v = 0;
                ok = r.Double(&v);
                s.marker_size = static_cast<float>(v);
                break;
              }
              case 22: {
                uint32_t tn;
                ok = r.Array(&tn);
                for (uint32_t t = 0; ok && t < tn; ++t) {
                  int64_t tv;
                  ok = r.Int(&tv);
                  s.totals.push_back(static_cast<int>(tv));
                }
                break;
              }
              case 9:
              case 10: {
                std::string b;
                ok = r.Bin(&b);
                std::vector<double> v(b.size() / sizeof(double));
                if (!v.empty()) std::memcpy(v.data(), b.data(), v.size() * sizeof(double));
                if (!py) py = std::make_shared<model::SeriesData>();
                (sk == 9 ? py->x : py->y) = std::move(v);
                break;
              }
              default: ok = r.Skip(); break;
            }
          }
          if (py && s.python_data) {
            py->version = 1;
            s.data = std::move(py);
          }
          ch->series.push_back(std::move(s));
        }
        break;
      }
      case 17: ok = r.IntAs(&ch->z); break;
      case 18: ok = r.Bool(&ch->created_by_python); break;
      case 19: ok = r.Bool(&ch->floating); break;
      case 22: ok = r.Double(&ch->bin_width); break;
      case 23: ok = r.IntAs(&ch->bin_count); break;
      case 24: ok = r.Str(&ch->source); break;
      case 25: ok = r.Bool(&ch->source_by_rows); break;
      case 29: ok = r.IntAs(&ch->gap_width); break;
      case 30: ok = r.IntAs(&ch->overlap); break;
      case 31: ok = r.IntAs(&ch->first_slice_angle); break;
      case 32: ok = r.IntAs(&ch->hole_size); break;
      case 33: ok = r.Bool(&ch->plot_visible_only); break;
      case 34: ok = r.IntAs(&ch->display_blanks); break;
      case 35: ok = r.Bool(&ch->na_as_blank); break;
      case 36: ok = r.Bool(&ch->title_overlay); break;
      case 37: ok = r.IntAs(&ch->style); break;
      case 38: ok = r.IntAs(&ch->color_scheme); break;
      case 39: {
        uint32_t m = 0;
        ok = r.Array(&m) && m >= 5 && r.Bool(&ch->bins_overflow) && r.Double(&ch->overflow_at) && r.Bool(&ch->bins_underflow) && r.Double(&ch->underflow_at) &&
             r.Bool(&ch->bins_by_category);
        for (uint32_t j = 5; ok && j < m; ++j) ok = r.Skip();
        break;
      }
      case 40: ok = r.Bool(&ch->connectors); break;
      case 41: ok = r.Bool(&ch->lock_aspect); break;
      case 50:
      case 51:
      case 52:
      case 53:
      case 54:
      case 55: ok = ReadChartTypeKey(r, k, ch); break;
      case 28: {
        uint32_t m = 0;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          int64_t hk = 0;
          ok = r.Int(&hk);
          if (ok && hk >= 0 && hk < 1000000) ch->hidden_cats.push_back(static_cast<int>(hk));
        }
        break;
      }
      case 27: {
        uint32_t m = 0;
        ok = r.Array(&m) && m >= 5 && r.Bool(&ch->view3d) && r.Double(&ch->rotation) && r.Double(&ch->elevation) && r.Double(&ch->perspective) &&
             r.Double(&ch->depth_percent);
        for (uint32_t j = 5; ok && j < m; ++j) ok = r.Skip();
        break;
      }
      case 26: {
        uint32_t m = 0;
        ok = r.Map(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          int64_t fk = 0;
          ok = r.Int(&fk);
          if (!ok) break;
          switch (fk) {
            case 1: ok = ReadChartFont(r, &ch->title_font); break;
            case 2: ok = ReadChartFont(r, &ch->legend_font); break;
            case 3: ok = ReadChartFont(r, &ch->axis_font); break;
            case 4: ok = r.IntAs(&ch->legend_pos); break;
            case 5: ok = r.IntAs(&ch->chart_fill); break;
            case 6: ok = r.IntAs(&ch->chart_line); break;
            case 7: ok = r.Bool(&ch->chart_line_none); break;
            case 8: ok = r.IntAs(&ch->plot_fill); break;
            case 9: ok = ReadAxisFormat(r, &ch->cat_axis); break;
            case 10: ok = ReadAxisFormat(r, &ch->val_axis, &axes_ext); break;
            case 11: ok = ReadAxisFormat(r, &ch->val_axis2); break;
            case 12: ok = r.IntAs(&ch->plot_line); break;
            case 13: ok = r.IntAs(&ch->legend_fill); break;
            case 14: ok = r.IntAs(&ch->legend_line); break;
            case 15: ok = r.IntAs(&ch->title_fill); break;
            case 16: ok = r.IntAs(&ch->title_line); break;
            case 17: ok = ReadManualRect(r, &ch->title_rect); break;
            case 18: ok = ReadManualRect(r, &ch->legend_rect); break;
            case 19: ok = ReadManualRect(r, &ch->plot_rect); break;
            case 20: ok = r.Bool(&ch->legend_overlay); break;
            default: ok = r.Skip(); break;
          }
        }
        break;
      }
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  if (!axes_ext) {  // an older file: one Automatic / Fixed pair per axis
    ch->val_axis.min_auto = ch->val_axis.max_auto = y_auto;
    ch->val_axis.min = y_min;
    ch->val_axis.max = y_max;
    ch->cat_axis.min_auto = ch->cat_axis.max_auto = x_auto;
    ch->cat_axis.min = x_min;
    ch->cat_axis.max = x_max;
  }
  return true;
}

std::string Compress(std::string_view raw, bool* compressed) {
  if (raw.size() < 256) {
    *compressed = false;
    return std::string(raw);
  }
  std::string out(ZSTD_compressBound(raw.size()), '\0');
  const size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 3);
  if (ZSTD_isError(n) || n >= raw.size()) {
    *compressed = false;
    return std::string(raw);
  }
  out.resize(n);
  *compressed = true;
  return out;
}

bool Decompress(std::string_view stored, uint64_t raw_size, std::string* out) {
  if (raw_size > (1ull << 30)) return false;  // a chunk never decompresses to more than 1 GB
  out->resize(static_cast<size_t>(raw_size));
  const size_t n = ZSTD_decompress(out->data(), out->size(), stored.data(), stored.size());
  return !ZSTD_isError(n) && n == raw_size;
}

// ---- Conditional formatting, data validation, AutoFilter (SHET 18-20) ----

void WriteRects(MsgWriter& w, const std::vector<Rect>& v) {
  w.Array(static_cast<uint32_t>(v.size()));
  for (const Rect& r : v) WriteRect(w, r);
}
bool ReadRects(MsgReader& r, std::vector<Rect>* v) {
  uint32_t n;
  if (!r.Array(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    Rect x;
    if (!ReadRect(r, &x)) return false;
    v->push_back(x);
  }
  return true;
}

void WriteCf(MsgWriter& w, const model::CfRule& c) {
  const bool bar_opts = c.bar_min_type >= 0 || c.bar_max_type >= 0 || !c.bar_gradient || !c.bar_show_value || c.bar_border || c.bar_direction;
  const bool fmt_extra = !c.fmt.numfmt.empty() || c.fmt.underline >= 0 || c.fmt.pattern >= 0;
  const bool sides = c.fmt.side_style[0] || c.fmt.side_style[1] || c.fmt.side_style[2] || c.fmt.side_style[3];
  w.Map(34 + (bar_opts ? 2u : 0u) + (fmt_extra ? 1u : 0u) + (sides ? 1u : 0u));
  w.KInt(1, c.id);
  w.Int(2);
  WriteRects(w, c.ranges);
  w.KInt(3, static_cast<int>(c.type));
  w.KInt(4, static_cast<int>(c.op));
  w.KStr(5, c.formula1);
  w.KStr(6, c.formula2);
  w.KStr(7, c.text);
  w.KBool(8, c.fmt.has_fill);
  w.KInt(9, c.fmt.fill_color);
  w.KBool(10, c.fmt.has_font_color);
  w.KInt(11, c.fmt.font_color);
  w.KInt(12, c.fmt.bold);
  w.KInt(13, c.fmt.italic);
  w.KInt(14, c.fmt.strike);
  w.KInt(15, c.rank);
  w.KBool(16, c.bottom);
  w.KBool(17, c.percent);
  w.KBool(18, c.below);
  w.KBool(19, c.duplicate);
  w.KInt(20, c.points);
  w.Int(21);
  w.Array(3);
  for (uint32_t col : c.colors) w.Int(col);
  w.KInt(22, c.bar_color);
  w.KInt(23, c.priority);
  w.KBool(24, c.stop_if_true);
  // icon sets
  w.KInt(25, c.icon_style);
  w.Int(26);
  w.Array(static_cast<uint32_t>(c.icon_thresholds.size()));
  for (const auto& t : c.icon_thresholds) {
    w.Array(4);
    w.Int(t.type);
    w.Double(t.value);
    w.Str(t.formula);
    w.Bool(t.gte);
  }
  w.KBool(27, c.icon_reverse);
  w.KBool(28, c.icon_only);
  // Excel parity (cell formatting): text operator, date period, average variants, color scale points
  w.KInt(29, c.text_op);
  w.KInt(30, c.time_period);
  w.KBool(31, c.equal_average);
  w.KInt(32, c.std_dev);
  w.Int(33);
  w.Array(3);
  for (int i = 0; i < 3; ++i) {
    w.Array(3);
    w.Int(c.point_type[i]);
    w.Double(c.point_value[i]);
    w.Str(c.point_formula[i]);
  }
  w.KInt(34, c.fmt.border_color);  // the format's outline (Red Border)
  if (bar_opts) {
    // 35: data bar look [gradient, show value, border color, direction]; 36: its bounds [[type, value, formula] x 2]
    w.Int(35);
    w.Array(4);
    w.Bool(c.bar_gradient);
    w.Bool(c.bar_show_value);
    w.Int(c.bar_border);
    w.Int(c.bar_direction);
    w.Int(36);
    w.Array(2);
    w.Array(3);
    w.Int(c.bar_min_type);
    w.Double(c.bar_min_value);
    w.Str(c.bar_min_formula);
    w.Array(3);
    w.Int(c.bar_max_type);
    w.Double(c.bar_max_value);
    w.Str(c.bar_max_formula);
  }
  if (fmt_extra) {
    // 37: the format's [number format code, underline, pattern, pattern color]
    w.Int(37);
    w.Array(4);
    w.Str(c.fmt.numfmt);
    w.Int(c.fmt.underline);
    w.Int(c.fmt.pattern);
    w.Int(c.fmt.pattern_color);
  }
  if (sides) {
    // 38: the format's lines [[line style, color] x left, top, right, bottom]
    w.Int(38);
    w.Array(4);
    for (int k = 0; k < 4; ++k) {
      w.Array(2);
      w.Int(c.fmt.side_style[k]);
      w.Int(c.fmt.side_color[k]);
    }
  }
}

bool ReadCf(MsgReader& r, model::CfRule* c) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  bool ok = true;
  for (uint32_t i = 0; ok && i < n; ++i) {
    int64_t k, iv = 0;
    if (!r.Int(&k)) return false;
    switch (k) {
      case 1: ok = r.IntAs(&c->id); break;
      case 2: ok = ReadRects(r, &c->ranges); break;
      case 3: ok = r.Int(&iv); c->type = static_cast<model::CfType>(iv); break;
      case 4: ok = r.Int(&iv); c->op = static_cast<model::CmpOp>(iv); break;
      case 5: ok = r.Str(&c->formula1); break;
      case 6: ok = r.Str(&c->formula2); break;
      case 7: ok = r.Str(&c->text); break;
      case 8: ok = r.Bool(&c->fmt.has_fill); break;
      case 9: ok = r.IntAs(&c->fmt.fill_color); break;
      case 10: ok = r.Bool(&c->fmt.has_font_color); break;
      case 11: ok = r.IntAs(&c->fmt.font_color); break;
      case 12: ok = r.IntAs(&c->fmt.bold); break;
      case 13: ok = r.IntAs(&c->fmt.italic); break;
      case 14: ok = r.IntAs(&c->fmt.strike); break;
      case 15: ok = r.IntAs(&c->rank); break;
      case 16: ok = r.Bool(&c->bottom); break;
      case 17: ok = r.Bool(&c->percent); break;
      case 18: ok = r.Bool(&c->below); break;
      case 19: ok = r.Bool(&c->duplicate); break;
      case 20: ok = r.IntAs(&c->points); break;
      case 21: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t q = 0; ok && q < m; ++q) {
          uint32_t col = 0;
          ok = r.IntAs(&col);
          if (q < 3) c->colors[q] = col;
        }
        break;
      }
      case 22: ok = r.IntAs(&c->bar_color); break;
      case 23: ok = r.IntAs(&c->priority); break;
      case 24: ok = r.Bool(&c->stop_if_true); break;
      case 25: ok = r.IntAs(&c->icon_style); break;
      case 26: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t q = 0; ok && q < m; ++q) {
          uint32_t f;
          model::IconThreshold t;
          int64_t ty = 0;
          ok = r.Array(&f) && f >= 4 && r.Int(&ty) && r.Double(&t.value) && r.Str(&t.formula) && r.Bool(&t.gte);
          for (uint32_t z = 4; ok && z < f; ++z) ok = r.Skip();
          t.type = static_cast<int>(ty);
          c->icon_thresholds.push_back(std::move(t));
        }
        break;
      }
      case 27: ok = r.Bool(&c->icon_reverse); break;
      case 28: ok = r.Bool(&c->icon_only); break;
      case 29: ok = r.IntAs(&c->text_op); break;
      case 30: ok = r.IntAs(&c->time_period); break;
      case 31: ok = r.Bool(&c->equal_average); break;
      case 32: ok = r.IntAs(&c->std_dev); break;
      case 33: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t q = 0; ok && q < m; ++q) {
          uint32_t f;
          int64_t ty = -1;
          double val = 0;
          std::string fx;
          ok = r.Array(&f) && f >= 3 && r.Int(&ty) && r.Double(&val) && r.Str(&fx);
          for (uint32_t z = 3; ok && z < f; ++z) ok = r.Skip();
          if (ok && q < 3) {
            c->point_type[q] = static_cast<int>(ty);
            c->point_value[q] = val;
            c->point_formula[q] = std::move(fx);
          }
        }
        break;
      }
      case 34: ok = r.IntAs(&c->fmt.border_color); break;
      case 35: {
        uint32_t m;
        int64_t bc = 0, dir = 0;
        ok = r.Array(&m) && m >= 4 && r.Bool(&c->bar_gradient) && r.Bool(&c->bar_show_value) && r.Int(&bc) && r.Int(&dir);
        for (uint32_t z = 4; ok && z < m; ++z) ok = r.Skip();
        c->bar_border = static_cast<uint32_t>(bc);
        c->bar_direction = static_cast<int>(std::clamp<int64_t>(dir, 0, 2));
        break;
      }
      case 36: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t q = 0; ok && q < m; ++q) {
          uint32_t f;
          int64_t ty = -1;
          double val = 0;
          std::string fx;
          ok = r.Array(&f) && f >= 3 && r.Int(&ty) && r.Double(&val) && r.Str(&fx);
          for (uint32_t z = 3; ok && z < f; ++z) ok = r.Skip();
          if (ok && q == 0) c->bar_min_type = static_cast<int>(ty), c->bar_min_value = val, c->bar_min_formula = std::move(fx);
          if (ok && q == 1) c->bar_max_type = static_cast<int>(ty), c->bar_max_value = val, c->bar_max_formula = std::move(fx);
        }
        break;
      }
      case 37: {
        uint32_t m;
        int64_t u = -1, pat = -1, pc = 0;
        ok = r.Array(&m) && m >= 4 && r.Str(&c->fmt.numfmt) && r.Int(&u) && r.Int(&pat) && r.Int(&pc);
        for (uint32_t z = 4; ok && z < m; ++z) ok = r.Skip();
        c->fmt.underline = static_cast<int8_t>(std::clamp<int64_t>(u, -1, 4));
        c->fmt.pattern = static_cast<int8_t>(std::clamp<int64_t>(pat, -1, static_cast<int64_t>(model::FillPattern::Count) - 1));
        c->fmt.pattern_color = static_cast<uint32_t>(pc);
        break;
      }
      case 38: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t q = 0; ok && q < m; ++q) {
          uint32_t f;
          int64_t st = 0, col = 0;
          ok = r.Array(&f) && f >= 2 && r.Int(&st) && r.Int(&col);
          for (uint32_t z = 2; ok && z < f; ++z) ok = r.Skip();
          if (ok && q < 4) c->fmt.side_style[q] = static_cast<uint8_t>(std::clamp<int64_t>(st, 0, 13)), c->fmt.side_color[q] = static_cast<uint32_t>(col);
        }
        break;
      }
      default: ok = r.Skip(); break;
    }
  }
  return ok;
}

void WriteDv(MsgWriter& w, const model::DvRule& d) {
  w.Map(14);
  w.Int(1);
  WriteRects(w, d.ranges);
  w.KInt(2, static_cast<int>(d.type));
  w.KInt(3, static_cast<int>(d.op));
  w.KStr(4, d.formula1);
  w.KStr(5, d.formula2);
  w.KBool(6, d.ignore_blank);
  w.KBool(7, d.in_cell_dropdown);
  w.KBool(8, d.show_input);
  w.KBool(9, d.show_error);
  w.KInt(10, static_cast<int>(d.alert));
  w.KStr(11, d.input_title);
  w.KStr(12, d.input_message);
  w.KStr(13, d.error_title);
  w.KStr(14, d.error_message);
}

bool ReadDv(MsgReader& r, model::DvRule* d) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  bool ok = true;
  for (uint32_t i = 0; ok && i < n; ++i) {
    int64_t k, iv = 0;
    if (!r.Int(&k)) return false;
    switch (k) {
      case 1: ok = ReadRects(r, &d->ranges); break;
      case 2: ok = r.Int(&iv); d->type = static_cast<model::DvType>(iv); break;
      case 3: ok = r.Int(&iv); d->op = static_cast<model::CmpOp>(iv); break;
      case 4: ok = r.Str(&d->formula1); break;
      case 5: ok = r.Str(&d->formula2); break;
      case 6: ok = r.Bool(&d->ignore_blank); break;
      case 7: ok = r.Bool(&d->in_cell_dropdown); break;
      case 8: ok = r.Bool(&d->show_input); break;
      case 9: ok = r.Bool(&d->show_error); break;
      case 10: ok = r.Int(&iv); d->alert = static_cast<model::DvAlert>(iv); break;
      case 11: ok = r.Str(&d->input_title); break;
      case 12: ok = r.Str(&d->input_message); break;
      case 13: ok = r.Str(&d->error_title); break;
      case 14: ok = r.Str(&d->error_message); break;
      default: ok = r.Skip(); break;
    }
  }
  return ok;
}

void WriteAf(MsgWriter& w, const model::AutoFilter& a) {
  w.Map(6);
  w.KBool(1, a.on);
  w.Int(2);
  WriteRect(w, a.range);
  w.Int(3);
  w.Array(static_cast<uint32_t>(a.columns.size()));
  for (const auto& c : a.columns) {
    w.Map(15);
    w.KInt(1, c.field);
    w.KInt(2, c.kind);
    w.Int(3);
    w.Array(static_cast<uint32_t>(c.values.size()));
    for (const auto& v : c.values) w.Str(v);
    w.KBool(4, c.blanks);
    w.KStr(5, c.crit1);
    w.KStr(6, c.crit2);
    w.KBool(7, c.or_op);
    w.KBool(8, c.top);
    w.KBool(9, c.percent);
    w.KInt(10, c.count);
    w.KInt(11, 0);
    w.KInt(12, c.color);
    w.KInt(13, c.dynamic);
    w.KInt(14, c.icon_set);  // kind 7 (Filter by Cell Icon)
    w.KInt(15, c.icon);
  }
  w.Int(4);
  w.Array(static_cast<uint32_t>(a.hidden.size()));
  for (const auto& [row, sz] : a.hidden) w.Int(row);
  // 5: the first sort level (older versions); 6: every level, [col, desc] each
  w.Int(5);
  w.Array(2);
  w.Int(a.sort_levels.empty() ? -1 : static_cast<int64_t>(a.sort_levels[0].col));
  w.Bool(!a.sort_levels.empty() && a.sort_levels[0].desc);
  w.Int(6);
  w.Array(static_cast<uint32_t>(a.sort_levels.size()));
  for (const auto& l : a.sort_levels) {
    w.Array(2);
    w.Int(l.col);
    w.Bool(l.desc);
  }
}

bool ReadAf(MsgReader& r, model::AutoFilter* a) {
  uint32_t n;
  if (!r.Map(&n)) return false;
  bool ok = true;
  for (uint32_t i = 0; ok && i < n; ++i) {
    int64_t k;
    if (!r.Int(&k)) return false;
    switch (k) {
      case 1: ok = r.Bool(&a->on); break;
      case 2: ok = ReadRect(r, &a->range); break;
      case 3: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          model::FilterColumn c;
          uint32_t f;
          ok = r.Map(&f);
          for (uint32_t q = 0; ok && q < f; ++q) {
            int64_t ck;
            ok = r.Int(&ck);
            if (!ok) break;
            switch (ck) {
              case 1: ok = r.IntAs(&c.field); break;
              case 2: ok = r.IntAs(&c.kind); break;
              case 3: {
                uint32_t vn;
                ok = r.Array(&vn);
                for (uint32_t z = 0; ok && z < vn; ++z) {
                  std::string sv;
                  ok = r.Str(&sv);
                  c.values.push_back(std::move(sv));
                }
                break;
              }
              case 4: ok = r.Bool(&c.blanks); break;
              case 5: ok = r.Str(&c.crit1); break;
              case 6: ok = r.Str(&c.crit2); break;
              case 7: ok = r.Bool(&c.or_op); break;
              case 8: ok = r.Bool(&c.top); break;
              case 9: ok = r.Bool(&c.percent); break;
              case 10: ok = r.IntAs(&c.count); break;
              case 12: ok = r.IntAs(&c.color); break;
              case 13: ok = r.IntAs(&c.dynamic); break;
              case 14: ok = r.IntAs(&c.icon_set); break;
              case 15: ok = r.IntAs(&c.icon); break;
              default: ok = r.Skip(); break;
            }
          }
          a->columns.push_back(std::move(c));
        }
        break;
      }
      case 4: {
        uint32_t m;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          uint32_t row = 0;
          ok = r.IntAs(&row);
          a->hidden[row] = 0;
        }
        break;
      }
      case 5: {
        uint32_t m;
        int field = -1;
        bool desc = false;
        ok = r.Array(&m) && m >= 2 && r.IntAs(&field) && r.Bool(&desc);
        for (uint32_t q = 2; ok && q < m; ++q) ok = r.Skip();
        if (ok && field >= 0 && a->sort_levels.empty()) a->sort_levels.push_back(model::SortLevel{static_cast<uint32_t>(field), desc});
        break;
      }
      case 6: {  // every sort level (replaces key 5's first one)
        uint32_t m;
        ok = r.Array(&m);
        a->sort_levels.clear();
        for (uint32_t j = 0; ok && j < m; ++j) {
          uint32_t e;
          model::SortLevel l;
          ok = r.Array(&e) && e >= 2 && r.IntAs(&l.col) && r.Bool(&l.desc);
          for (uint32_t q = 2; ok && q < e; ++q) ok = r.Skip();
          if (ok) a->sort_levels.push_back(l);
        }
        break;
      }
      default: ok = r.Skip(); break;
    }
  }
  return ok;
}

// ---- PivotTables: integer-keyed maps after each record's positional elements (readers skip keys they don't know) ----
void WriteStrList(MsgWriter& w, const std::vector<std::string>& v) {
  w.Array(static_cast<uint32_t>(v.size()));
  for (const auto& x : v) w.Str(x);
}

bool ReadStrList(MsgReader& r, std::vector<std::string>* v) {
  uint32_t n = 0;
  if (!r.Array(&n)) return false;
  v->clear();
  for (uint32_t i = 0; i < n; ++i) {
    std::string x;
    if (!r.Str(&x)) return false;
    v->push_back(std::move(x));
  }
  return true;
}

void WritePivotFilter(MsgWriter& w, const model::PivotFilter& f) {
  w.Array(4);
  w.Int(f.type);
  w.Int(f.data);
  w.Str(f.v1);
  w.Str(f.v2);
}

bool ReadPivotFilter(MsgReader& r, model::PivotFilter* f) {
  uint32_t n = 0;
  if (!r.Array(&n) || n < 4 || !r.IntAs(&f->type) || !r.IntAs(&f->data) || !r.Str(&f->v1) || !r.Str(&f->v2)) return false;
  for (uint32_t i = 4; i < n; ++i) {
    if (!r.Skip()) return false;
  }
  return true;
}

// a map of the entries `body` writes (it returns how many)
void WriteKeyMap(MsgWriter& w, const std::function<uint32_t(MsgWriter&)>& body) {
  MsgWriter tmp;
  const uint32_t n = body(tmp);
  w.Map(n);
  w.Raw(tmp.buf());
}

// axis field element 5: 1 collapsed, 2 collapse_all, 3 sort_by, 4 sort_col, 5 multi_page, 6 label filter, 7 value filter,
// 8 caption, 9 subtotal functions, 10 repeat labels, 11 blank line, 12 show items with no data, 13 manual order,
// 14 renamed items [item, name, item, name...], 15 include new items in manual filter, 16 [items when the manual filter
// was set], 17 layout form (0 compact, 1 outline, 2 tabular), 18 subtotals at the top (0 / 1), 19 page break after each item
void WritePivotAxisExt(MsgWriter& w, const model::PivotAxisField& f) {
  WriteKeyMap(w, [&](MsgWriter& m) {
    uint32_t n = 0;
    if (!f.collapsed.empty()) m.Int(1), WriteStrList(m, f.collapsed), ++n;
    if (f.collapse_all) m.KBool(2, true), ++n;
    if (f.sort_by >= 0) m.KInt(3, f.sort_by), ++n;
    if (!f.sort_col.empty()) m.Int(4), WriteStrList(m, f.sort_col), ++n;
    if (f.multi_page) m.KBool(5, true), ++n;
    if (f.label_filter.type) m.Int(6), WritePivotFilter(m, f.label_filter), ++n;
    if (f.value_filter.type) m.Int(7), WritePivotFilter(m, f.value_filter), ++n;
    if (!f.caption.empty()) m.KStr(8, f.caption), ++n;
    if (f.subtotal_funcs) m.KInt(9, f.subtotal_funcs), ++n;
    if (f.repeat_labels) m.KBool(10, true), ++n;
    if (f.blank_line) m.KBool(11, true), ++n;
    if (f.show_all_items) m.KBool(12, true), ++n;
    if (!f.manual_order.empty()) m.Int(13), WriteStrList(m, f.manual_order), ++n;
    if (f.include_new) m.KBool(15, true), ++n;
    if (!f.known.empty()) m.Int(16), WriteStrList(m, f.known), ++n;
    if (f.form >= 0) m.KInt(17, f.form), ++n;
    if (f.sub_top >= 0) m.KInt(18, f.sub_top), ++n;
    if (f.page_break) m.KBool(19, true), ++n;
    if (!f.item_captions.empty()) {
      m.Int(14);
      m.Array(static_cast<uint32_t>(f.item_captions.size() * 2));
      for (const auto& [from, to] : f.item_captions) m.Str(from), m.Str(to);
      ++n;
    }
    return n;
  });
}

bool ReadPivotAxisExt(MsgReader& r, model::PivotAxisField* f) {
  uint32_t n = 0;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1: ok = ReadStrList(r, &f->collapsed); break;
      case 2: ok = r.Bool(&f->collapse_all); break;
      case 3: ok = r.IntAs(&f->sort_by); break;
      case 4: ok = ReadStrList(r, &f->sort_col); break;
      case 5: ok = r.Bool(&f->multi_page); break;
      case 6: ok = ReadPivotFilter(r, &f->label_filter); break;
      case 7: ok = ReadPivotFilter(r, &f->value_filter); break;
      case 8: ok = r.Str(&f->caption); break;
      case 9: ok = r.IntAs(&f->subtotal_funcs); break;
      case 10: ok = r.Bool(&f->repeat_labels); break;
      case 11: ok = r.Bool(&f->blank_line); break;
      case 12: ok = r.Bool(&f->show_all_items); break;
      case 13: ok = ReadStrList(r, &f->manual_order); break;
      case 14: {
        std::vector<std::string> v;
        ok = ReadStrList(r, &v);
        for (size_t j = 0; ok && j + 1 < v.size(); j += 2) f->item_captions.emplace_back(v[j], v[j + 1]);
        break;
      }
      case 15: ok = r.Bool(&f->include_new); break;
      case 16: ok = ReadStrList(r, &f->known); break;
      case 17: ok = r.IntAs(&f->form); break;
      case 18: ok = r.IntAs(&f->sub_top); break;
      case 19: ok = r.Bool(&f->page_break); break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

// data field element 5: 1 Show Values As (all 15 kinds; element 2 keeps what older readers know), 2 base field, 3 base item
void WritePivotDataExt(MsgWriter& w, const model::PivotDataField& d) {
  WriteKeyMap(w, [&](MsgWriter& m) {
    uint32_t n = 0;
    if (d.show_as != model::PivotShowAs::Normal) m.KInt(1, static_cast<int>(d.show_as)), ++n;
    if (d.base_field >= 0) m.KInt(2, d.base_field), ++n;
    if (!d.base_item.empty()) m.KStr(3, d.base_item), ++n;
    return n;
  });
}

bool ReadPivotDataExt(MsgReader& r, model::PivotDataField* d) {
  uint32_t n = 0;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k = 0, v = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1:
        ok = r.Int(&v);
        d->show_as = static_cast<model::PivotShowAs>(std::clamp<int64_t>(v, 0, model::kPivotShowAs - 1));
        break;
      case 2: ok = r.IntAs(&d->base_field); break;
      case 3: ok = r.Str(&d->base_item); break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

// group element 5: 1 periods, 2 days, 3 auto start, 4 auto end, 5 name, 6 groups of items [[name, [items]]], 7 placed,
// 8 the kind (element 1 keeps a kind older readers know)
void WritePivotGroupExt(MsgWriter& w, const model::PivotGroup& g) {
  WriteKeyMap(w, [&](MsgWriter& m) {
    uint32_t n = 0;
    if (g.periods) m.KInt(1, g.periods), ++n;
    if (g.days != 1) m.KInt(2, g.days), ++n;
    if (g.auto_start) m.KBool(3, true), ++n;
    if (g.auto_end) m.KBool(4, true), ++n;
    if (!g.name.empty()) m.KStr(5, g.name), ++n;
    if (!g.named.empty()) {
      m.Int(6);
      m.Array(static_cast<uint32_t>(g.named.size()));
      for (const auto& [name, items] : g.named) {
        m.Array(2);
        m.Str(name);
        WriteStrList(m, items);
      }
      ++n;
    }
    if (g.placed) m.KBool(7, true), ++n;
    m.KInt(8, static_cast<int>(g.by)), ++n;
    return n;
  });
}

bool ReadPivotGroupExt(MsgReader& r, model::PivotGroup* g) {
  uint32_t n = 0;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k = 0, v = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1: ok = r.IntAs(&g->periods); break;
      case 2: ok = r.IntAs(&g->days); break;
      case 3: ok = r.Bool(&g->auto_start); break;
      case 4: ok = r.Bool(&g->auto_end); break;
      case 5: ok = r.Str(&g->name); break;
      case 6: {
        uint32_t m = 0;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          uint32_t two = 0;
          std::pair<std::string, std::vector<std::string>> e;
          ok = r.Array(&two) && two >= 2 && r.Str(&e.first) && ReadStrList(r, &e.second);
          for (uint32_t y = 2; ok && y < two; ++y) ok = r.Skip();
          g->named.push_back(std::move(e));
        }
        break;
      }
      case 7: ok = r.Bool(&g->placed); break;
      case 8:
        ok = r.Int(&v);
        g->by = static_cast<model::PivotGroupBy>(std::clamp<int64_t>(v, 0, static_cast<int64_t>(model::PivotGroupBy::Items)));
        break;
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  return true;
}

// pivot element 21: PivotTable Options, Style Options and the formats kept across refreshes
void WritePivotOptions(MsgWriter& w, const model::PivotTable& p, const model::StyleTable& styles) {
  WriteKeyMap(w, [&](MsgWriter& m) {
    uint32_t n = 0;
    if (p.merge_labels) m.KBool(1, true), ++n;
    if (p.indent != 1) m.KInt(2, p.indent), ++n;
    if (p.show_error) m.KBool(3, true), ++n;
    if (!p.error_string.empty()) m.KStr(4, p.error_string), ++n;
    if (p.show_null) m.KBool(5, true), ++n;
    if (!p.null_string.empty()) m.KStr(6, p.null_string), ++n;
    if (!p.autofit) m.KBool(7, false), ++n;
    if (!p.preserve_formatting) m.KBool(8, false), ++n;
    if (!p.show_buttons) m.KBool(9, false), ++n;
    if (!p.field_captions) m.KBool(10, false), ++n;
    if (p.refresh_on_open) m.KBool(11, true), ++n;
    if (!p.enable_drilldown) m.KBool(12, false), ++n;
    if (p.field_list_order) m.KInt(13, p.field_list_order), ++n;
    if (!p.custom_lists) m.KBool(14, false), ++n;
    if (!p.chart_buttons) m.KBool(15, false), ++n;
    if (!p.style_row_headers) m.KBool(16, false), ++n;
    if (!p.style_col_headers) m.KBool(17, false), ++n;
    if (p.style_banded_rows) m.KBool(18, true), ++n;
    if (p.style_banded_cols) m.KBool(19, true), ++n;
    if (p.general_values) m.KBool(22, true), ++n;
    if (!p.enable_selection) m.KBool(23, false), ++n;
    if (!p.grand_name.empty()) m.KStr(25, p.grand_name), ++n;
    if (p.refreshed_at != 0) m.Int(26), m.Double(p.refreshed_at), ++n;
    if (!p.refreshed_by.empty()) m.KStr(27, p.refreshed_by), ++n;
    if (!p.breaks.empty()) {
      m.Int(24);
      m.Array(static_cast<uint32_t>(p.breaks.size()));
      for (uint32_t b : p.breaks) m.Int(b);
      ++n;
    }
    if (!p.formats.empty()) {
      m.Int(20);
      m.Array(static_cast<uint32_t>(p.formats.size()));
      for (const auto& f : p.formats) {
        m.Array(3);
        m.Str(f.row);
        m.Str(f.col);
        WriteStyle(m, styles.Get(f.style), styles);
      }
      ++n;
    }
    return n;
  });
}

bool ReadPivotOptions(MsgReader& r, model::PivotTable* p, model::StyleTable& styles) {
  uint32_t n = 0;
  if (!r.Map(&n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t k = 0;
    if (!r.Int(&k)) return false;
    bool ok = true;
    switch (k) {
      case 1: ok = r.Bool(&p->merge_labels); break;
      case 2: ok = r.IntAs(&p->indent); break;
      case 3: ok = r.Bool(&p->show_error); break;
      case 4: ok = r.Str(&p->error_string); break;
      case 5: ok = r.Bool(&p->show_null); break;
      case 6: ok = r.Str(&p->null_string); break;
      case 7: ok = r.Bool(&p->autofit); break;
      case 8: ok = r.Bool(&p->preserve_formatting); break;
      case 9: ok = r.Bool(&p->show_buttons); break;
      case 10: ok = r.Bool(&p->field_captions); break;
      case 11: ok = r.Bool(&p->refresh_on_open); break;
      case 12: ok = r.Bool(&p->enable_drilldown); break;
      case 13: ok = r.IntAs(&p->field_list_order); break;
      case 14: ok = r.Bool(&p->custom_lists); break;
      case 15: ok = r.Bool(&p->chart_buttons); break;
      case 16: ok = r.Bool(&p->style_row_headers); break;
      case 17: ok = r.Bool(&p->style_col_headers); break;
      case 18: ok = r.Bool(&p->style_banded_rows); break;
      case 19: ok = r.Bool(&p->style_banded_cols); break;
      case 22: ok = r.Bool(&p->general_values); break;
      case 23: ok = r.Bool(&p->enable_selection); break;
      case 25: ok = r.Str(&p->grand_name); break;
      case 26: ok = r.Double(&p->refreshed_at); break;
      case 27: ok = r.Str(&p->refreshed_by); break;
      case 24: {
        uint32_t m = 0;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          uint32_t b = 0;
          ok = r.IntAs(&b);
          p->breaks.push_back(b);
        }
        break;
      }
      case 20: {
        uint32_t m = 0;
        ok = r.Array(&m);
        for (uint32_t j = 0; ok && j < m; ++j) {
          uint32_t three = 0;
          model::PivotFormat f;
          model::Style st;
          ok = r.Array(&three) && three >= 3 && r.Str(&f.row) && r.Str(&f.col) && ReadStyle(r, &st, styles);
          for (uint32_t y = 3; ok && y < three; ++y) ok = r.Skip();
          if (ok) {
            f.style = styles.Intern(st);
            p->formats.push_back(std::move(f));
          }
        }
        break;
      }
      default: ok = r.Skip(); break;
    }
    if (!ok) return false;
  }
  p->indent = std::clamp(p->indent, 0, 15);
  return true;
}

}  // namespace

std::string SaveYcgToBytes(const model::Workbook& wb, const SaveOptions& opt) {
  std::vector<ChunkOut> chunks;
  auto add = [&](uint32_t fourcc, std::string data) {
    const KnownChunk* k = Known(fourcc);
    chunks.push_back(ChunkOut{fourcc, k ? k->version : uint16_t{1}, k ? k->flags : uint16_t{0}, std::move(data)});
  };
  const model::StyleTable& styles = *wb.styles;

  // META
  {
    MsgWriter w;
    w.Map(3);
    w.KStr(1, "YCGrid");
    w.KStr(2, opt.app_version);
    w.KInt(3, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    add(kMETA, std::move(w.buf()));
  }
  // WKBK
  {
    MsgWriter w;
    const bool opts = wb.date1904 || wb.precision_as_displayed || wb.force_full_calc;
    w.Map(6 + (wb.calc_iterate >= 0 ? 1 : 0) + (opts ? 1 : 0));
    w.KInt(6, 1);  // formula dialect: 1 = DECIMAL with two arguments is Excel's base conversion
    w.Int(1);
    w.Array(static_cast<uint32_t>(wb.sheets.size()));
    for (const auto& s : wb.sheets) w.Int(s->id);
    w.KInt(2, wb.active_sheet);
    w.KInt(3, wb.next_sheet_id);
    w.KInt(4, wb.next_object_id);
    // workbook protection [structure, windows, hash]
    w.Int(5);
    w.Array(3);
    w.Bool(wb.protect_structure);
    w.Bool(wb.protect_windows);
    w.Int(wb.protect_hash);
    // calculation options [iterative on, maximum iterations, maximum change, mode (0 automatic, 1 manual, 2 automatic
    // except for data tables)]
    if (wb.calc_iterate >= 0) {
      w.Int(7);
      w.Array(wb.calc_mode >= 0 ? 4 : 3);
      w.Bool(wb.calc_iterate > 0);
      w.Int(wb.calc_max_iterations);
      w.Double(wb.calc_max_change);
      if (wb.calc_mode >= 0) w.Int(wb.calc_mode);
    }
    // the workbook's calculation options [1904 date system, precision as displayed, force full calculation]
    if (opts) {
      w.Int(8);
      w.Array(3);
      w.Bool(wb.date1904);
      w.Bool(wb.precision_as_displayed);
      w.Bool(wb.force_full_calc);
    }
    add(kWKBK, std::move(w.buf()));
  }
  // STYL: only the styles actually used (renumbered), id 0 is always the default style
  std::unordered_map<uint32_t, uint32_t> style_map;  // workbook id -> file index
  std::vector<uint32_t> style_order{0};
  style_map[0] = 0;
  auto use_style = [&](uint32_t id) {
    auto it = style_map.find(id);
    if (it != style_map.end()) return it->second;
    const uint32_t idx = static_cast<uint32_t>(style_order.size());
    style_map.emplace(id, idx);
    style_order.push_back(id);
    return idx;
  };
  // formulas: one record per def (A1 text stored relative to the first cell that uses it)
  std::unordered_map<uint32_t, uint32_t> fmla_map;  // def id -> file index (1-based)
  MsgWriter fw;
  std::string fmla_items;
  uint32_t fmla_count = 0;
  engine::WorkbookResolver res(wb);
  // CELL (one per sheet)
  std::vector<std::string> cell_chunks;
  for (const auto& sc : wb.sheets) {
    const Sheet& s = sc.get();
    std::string out;
    PutVar(out, s.id);
    // collect every non-blank cell (value, formula, style), sorted by (row, col)
    std::map<uint64_t, std::tuple<const Value*, uint32_t, uint32_t>> cells;
    s.values.ForEach([&](uint32_t rt, uint32_t ct, const model::ValueTile& t) {
      if (!t.count) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (!t.Occupied(idx)) continue;
        const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
        std::get<0>(cells[(uint64_t{r} << 16) | c]) = &t.v[idx];
      }
    });
    s.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (!t.formula_count && !t.style_count) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (!t.formula[idx] && !t.style[idx]) continue;
        const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
        auto& e = cells[(uint64_t{r} << 16) | c];
        std::get<1>(e) = t.formula[idx];
        std::get<2>(e) = t.style[idx];
      }
    });
    PutVar(out, cells.size());
    uint32_t prev_r = 0;
    for (const auto& [key, e] : cells) {
      const uint32_t r = static_cast<uint32_t>(key >> 16), c = static_cast<uint32_t>(key & 0xFFFF);
      const auto& [val, fid, sid] = e;
      PutVar(out, r - prev_r);
      prev_r = r;
      PutVar(out, c);
      uint8_t flags = 0;
      if (val && !val->IsEmpty()) flags |= 1;
      if (fid) flags |= 2;
      if (sid) flags |= 4;
      out += static_cast<char>(flags);
      if (flags & 1) PutValue(out, *val);
      if (flags & 2) {
        auto it = fmla_map.find(fid);
        if (it == fmla_map.end()) {
          const auto* f = static_cast<const formula::CompiledFormula*>(wb.formulas->Get(fid));
          const std::string text = f ? formula::ToA1(f->root, core::CellPos{r, c}, &res) : std::string("#REF!");
          MsgWriter iw;
          iw.Array(4);
          iw.Int(s.id);
          iw.Int(r);
          iw.Int(c);
          iw.Str(text);
          fmla_items += iw.buf();
          it = fmla_map.emplace(fid, ++fmla_count).first;
        }
        PutVar(out, it->second);
      }
      if (flags & 4) PutVar(out, use_style(sid));
    }
    cell_chunks.push_back(std::move(out));
  }
  // entire row / column styles go into the style table too
  for (const auto& sc : wb.sheets) {
    for (const auto& [k, v] : *sc->row_styles) use_style(v);
    for (const auto& [k, v] : *sc->col_styles) use_style(v);
  }
  {
    MsgWriter w;
    w.Map(3);
    w.KStr(1, styles.default_font);
    w.KDouble(2, styles.default_font_size);
    w.Int(3);
    w.Array(static_cast<uint32_t>(style_order.size()));
    for (uint32_t id : style_order) WriteStyle(w, styles.Get(id), styles);
    add(kSTYL, std::move(w.buf()));
  }
  // SHET
  for (const auto& sc : wb.sheets) {
    const Sheet& s = sc.get();
    MsgWriter w;
    w.Map(42 + (s.background->empty() ? 0u : 1u));  // key 100 (Page Layout > Background) only when set
    w.KBool(34, s.chart_sheet);
    // 35: outline settings (bit 0 summary rows above detail, bit 1 summary columns left of detail, bit 2 automatic styles)
    w.KInt(35, (s.outline_below ? 0 : 1) | (s.outline_right ? 0 : 2) | (s.outline_styles ? 4 : 0));
    w.KInt(1, s.id);
    w.KStr(2, s.name);
    w.KStr(3, s.code_name);
    w.KInt(4, s.row_limit);
    w.KInt(5, s.col_limit);
    w.KBool(6, s.visible);
    w.KBool(72, s.very_hidden);  // xlSheetVeryHidden
    w.KInt(7, s.tab_color);
    w.Int(8);
    WriteDims(w, *s.rows);
    w.Int(9);
    WriteDims(w, *s.cols);
    w.Int(10);
    {
      // in reading order: the same merged areas are the same bytes, whatever order edits and Undo left them in
      std::vector<Rect> merges = *s.merges;
      std::sort(merges.begin(), merges.end(), [](const Rect& a, const Rect& b) { return a.r0 != b.r0 ? a.r0 < b.r0 : a.c0 < b.c0; });
      w.Array(static_cast<uint32_t>(merges.size()));
      for (const Rect& m : merges) WriteRect(w, m);
    }
    w.Int(11);
    w.Map(static_cast<uint32_t>(s.row_styles->size()));
    for (const auto& [k, v] : *s.row_styles) {
      w.Int(k);
      w.Int(style_map[v]);
    }
    w.Int(12);
    w.Map(static_cast<uint32_t>(s.col_styles->size()));
    for (const auto& [k, v] : *s.col_styles) {
      w.Int(k);
      w.Int(style_map[v]);
    }
    w.Int(13);
    w.Array(static_cast<uint32_t>(s.notes->size()));
    for (const auto& [k, n] : *s.notes) {
      // 4-7: threaded comments (older versions read the first post as a note); 8: the note box's look {1: w, 2: h, 3: dx,
      // 4: dy, 5: font, 6: size, 7: bold, 8: italic, 9: font color, 10: fill, 11: line, 12: horizontal, 13: vertical}
      const bool look = !n.threaded && !(n.look == model::CellNote::Look{});
      w.Array(look ? 9 : n.threaded ? 8 : 4);
      w.Int(static_cast<int64_t>(k >> 16));
      w.Int(static_cast<int64_t>(k & 0xFFFF));
      w.Str(n.author);
      w.Str(n.text);
      if (look) {
        w.Bool(false);
        w.Double(0);
        w.Bool(false);
        w.Array(0);
        const auto& lk = n.look;
        w.Map(13);
        w.KDouble(1, lk.w);
        w.KDouble(2, lk.h);
        w.KDouble(3, lk.dx);
        w.KDouble(4, lk.dy);
        w.KStr(5, lk.font_name);
        w.KDouble(6, lk.font_size);
        w.KBool(7, lk.bold);
        w.KBool(8, lk.italic);
        w.KInt(9, lk.font_color);
        w.KInt(10, lk.fill_color);
        w.KInt(11, lk.line_color);
        w.KInt(12, lk.halign);
        w.KInt(13, lk.valign);
      }
      if (n.threaded) {
        w.Bool(true);
        w.Double(n.date);
        w.Bool(n.resolved);
        w.Array(static_cast<uint32_t>(n.replies.size()));
        for (const auto& rp : n.replies) {
          w.Array(3);
          w.Str(rp.author);
          w.Str(rp.text);
          w.Double(rp.date);
        }
      }
    }
    w.Int(14);
    WriteStringMap(w, *s.store, true);
    w.Int(15);
    w.Array(static_cast<uint32_t>(s.controls->size()));
    for (const auto& c : *s.controls) WriteControl(w, c);
    w.Int(16);
    w.Array(static_cast<uint32_t>(s.charts->size()));
    for (const auto& c : *s.charts) WriteChart(w, c);
    w.Int(17);
    w.Array(6);
    w.Int(s.view.top_row);
    w.Int(s.view.left_col);
    w.Int(s.view.act_row);
    w.Int(s.view.act_col);
    w.Int(s.view.freeze_row);
    w.Int(s.view.freeze_col);
    w.Int(18);
    w.Array(static_cast<uint32_t>(s.cond_formats->size()));
    for (const auto& c : *s.cond_formats) WriteCf(w, c);
    w.Int(19);
    w.Array(static_cast<uint32_t>(s.validations->size()));
    for (const auto& d : *s.validations) WriteDv(w, d);
    w.Int(20);
    WriteAf(w, *s.autofilter);
    w.Int(21);
    w.Array(static_cast<uint32_t>(s.links->size()));
    for (const auto& [k, l] : *s.links) {
      w.Array(5);
      w.Int(static_cast<int64_t>(k >> 16));
      w.Int(static_cast<int64_t>(k & 0xFFFF));
      w.Str(l.address);
      w.Str(l.sub_address);
      w.Str(l.tip);
    }
    // spill ranges of dynamic arrays and array formulas: [anchor row, anchor col, rows, cols, cse]
    w.Int(22);
    w.Array(static_cast<uint32_t>(s.spills->size()));
    for (const auto& [k, sp] : *s.spills) {
      w.Array(5);
      w.Int(static_cast<int64_t>(k >> 16));
      w.Int(static_cast<int64_t>(k & 0xFFFF));
      w.Int(static_cast<int64_t>(sp.rect.Rows()));
      w.Int(static_cast<int64_t>(sp.rect.Cols()));
      w.Bool(sp.cse);
    }
    // sheet protection: [on, hash, allow flags (15: Edit scenarios; older versions read the first 14), [[title, r0, c0, r1, c1, hash]]]
    {
      const auto& pr = *s.protection;
      w.Int(23);
      w.Array(4);
      w.Bool(pr.on);
      w.Int(pr.hash);
      const bool flags[] = {pr.select_locked, pr.select_unlocked, pr.format_cells, pr.format_columns, pr.format_rows, pr.insert_columns, pr.insert_rows,
                            pr.insert_hyperlinks, pr.delete_columns, pr.delete_rows, pr.sort, pr.autofilter, pr.pivot, pr.objects, pr.scenarios};
      w.Array(static_cast<uint32_t>(std::size(flags)));
      for (bool f : flags) w.Bool(f);
      w.Array(static_cast<uint32_t>(pr.ranges.size()));
      for (const auto& er : pr.ranges) {
        w.Array(er.users.empty() ? 6 : 7);  // 7: Permissions (older versions skip it)
        w.Str(er.title);
        w.Int(er.rect.r0);
        w.Int(er.rect.c0);
        w.Int(er.rect.r1);
        w.Int(er.rect.c1);
        w.Int(er.hash);
        if (!er.users.empty()) {
          w.Array(static_cast<uint32_t>(er.users.size()));
          for (const auto& u : er.users) {
            w.Array(2);
            w.Str(u.name);
            w.Bool(u.allow);
          }
        }
      }
    }
    // tables: [id, name, [r0, c0, r1, c1], style, flags bits, [[column name, totals function, totals label]]]
    w.Int(24);
    w.Array(static_cast<uint32_t>(s.tables->size()));
    for (const auto& t : *s.tables) {
      w.Array(7);  // 7: the table's own AutoFilter (older versions skip it)
      w.Int(t.id);
      w.Str(t.name);
      WriteRect(w, t.range);
      w.Str(t.style);
      w.Int((t.header ? 1 : 0) | (t.totals ? 2 : 0) | (t.banded_rows ? 4 : 0) | (t.banded_cols ? 8 : 0) | (t.first_col ? 16 : 0) | (t.last_col ? 32 : 0) |
            (t.filter_button ? 64 : 0));
      w.Array(static_cast<uint32_t>(t.columns.size()));
      for (const auto& c : t.columns) {
        w.Array(3);
        w.Str(c.name);
        w.Int(c.totals_func);
        w.Str(c.totals_label);
      }
      WriteAf(w, t.filter);
    }
    // shapes: [id, kind, name, [l, t, w, h], placement, [fill on, line on, fill, line, weight, flip h, flip v],
    //          [text, font, size, color, bold, italic, halign, valign], image, on_action, visible, z]
    w.Int(25);
    w.Array(static_cast<uint32_t>(s.shapes->size()));
    for (const auto& sh : *s.shapes) {
      w.Array(13);  // header 12 (with 11 elements) is taken by old files: #12 is a map of the newer fields, #13 is reserved (nil)
      w.Int(sh.id);
      w.Int(static_cast<int>(sh.kind));
      w.Str(sh.name);
      w.Array(4);
      w.Double(sh.left);
      w.Double(sh.top);
      w.Double(sh.width);
      w.Double(sh.height);
      w.Int(static_cast<int>(sh.placement));
      w.Array(7);
      w.Bool(sh.fill_visible);
      w.Bool(sh.line_visible);
      w.Int(sh.fill_color);
      w.Int(sh.line_color);
      w.Double(sh.line_weight);
      w.Bool(sh.flip_h);
      w.Bool(sh.flip_v);
      w.Array(8);
      w.Str(sh.text);
      w.Str(sh.font_name);
      w.Double(sh.font_size);
      w.Int(sh.font_color);
      w.Bool(sh.bold);
      w.Bool(sh.italic);
      w.Int(sh.halign);
      w.Int(sh.valign);
      w.Bin(sh.image);
      w.Str(sh.on_action);
      w.Bool(sh.visible);
      w.Int(sh.z);
      WriteShapeExtra(w, sh);
    }
    // sparklines: [id, type, [[r, c, src sheet, r0, c0, r1, c1]], [9 colors], flags, weight, {1: empty cells as,
    // 2: show hidden, 3: min scale, 4: max scale, 5: min, 6: max, 7: right to left, 8: date range, 9: axis color}]
    w.Int(26);
    w.Array(static_cast<uint32_t>(s.sparklines->size()));
    for (const auto& g : *s.sparklines) {
      w.Array(7);
      w.Int(g.id);
      w.Int(g.type);
      w.Array(static_cast<uint32_t>(g.items.size()));
      for (const auto& it : g.items) {
        w.Array(7);
        w.Int(it.r);
        w.Int(it.c);
        w.Int(it.src_sheet);
        w.Int(it.src.r0);
        w.Int(it.src.c0);
        w.Int(it.src.r1);
        w.Int(it.src.c1);
      }
      w.Array(7);
      for (uint32_t col : {g.color, g.negative_color, g.marker_color, g.high_color, g.low_color, g.first_color, g.last_color}) w.Int(col);
      w.Int((g.high ? 1 : 0) | (g.low ? 2 : 0) | (g.first ? 4 : 0) | (g.last ? 8 : 0) | (g.negative ? 16 : 0) | (g.markers ? 32 : 0) | (g.axis ? 64 : 0));
      w.Double(g.line_weight);
      w.Map(9);
      w.KInt(1, g.display_blanks);
      w.KBool(2, g.show_hidden);
      w.KInt(3, g.min_scale);
      w.KInt(4, g.max_scale);
      w.KDouble(5, g.min_value);
      w.KDouble(6, g.max_value);
      w.KBool(7, g.right_to_left);
      w.KStr(8, g.date_range);
      w.KInt(9, g.axis_color);
    }
    // page setup: [landscape, paper, [6 margins], zoom, fit wide, fit tall, [has area, r0, c0, r1, c1], [has title rows, r0, r1],
    //              [has title columns, c0, c1], flag bits, first page number, [6 header / footer texts], [row breaks], [column breaks]]
    {
      const auto& ps = *s.page_setup;
      w.Int(27);
      w.Array(ps.HasExtras() ? 15 : 14);
      w.Bool(ps.landscape);
      w.Int(ps.paper);
      w.Array(6);
      for (double m : {ps.left, ps.right, ps.top, ps.bottom, ps.header, ps.footer}) w.Double(m);
      w.Int(ps.zoom);
      w.Int(ps.fit_wide);
      w.Int(ps.fit_tall);
      w.Array(5);
      w.Bool(ps.has_area);
      w.Int(ps.area.r0);
      w.Int(ps.area.c0);
      w.Int(ps.area.r1);
      w.Int(ps.area.c1);
      w.Array(3);
      w.Bool(ps.has_title_rows);
      w.Int(ps.title_r0);
      w.Int(ps.title_r1);
      w.Array(3);
      w.Bool(ps.has_title_cols);
      w.Int(ps.title_c0);
      w.Int(ps.title_c1);
      w.Int((ps.gridlines ? 1 : 0) | (ps.headings ? 2 : 0) | (ps.center_h ? 4 : 0) | (ps.center_v ? 8 : 0) | (ps.black_and_white ? 16 : 0) |
            (ps.over_then_down ? 32 : 0) | (ps.different_first ? 64 : 0) | (ps.different_odd_even ? 128 : 0) | (ps.no_scale_with_doc ? 256 : 0) |
            (ps.no_align_margins ? 512 : 0));
      w.Int(ps.first_page_number);
      // the six texts, then (when there are any) the first page's six and the even pages' six: old readers keep the first six
      bool extra = false;
      for (int k = 0; k < 6; ++k) extra = extra || !ps.first_hf[k].empty() || !ps.even_hf[k].empty();
      w.Array(extra ? 18 : 6);
      for (const std::string* t : {&ps.header_left, &ps.header_center, &ps.header_right, &ps.footer_left, &ps.footer_center, &ps.footer_right}) w.Str(*t);
      if (extra) {
        for (const std::string& t : ps.first_hf) w.Str(t);
        for (const std::string& t : ps.even_hf) w.Str(t);
      }
      w.Array(static_cast<uint32_t>(ps.row_breaks.size()));
      for (uint32_t b : ps.row_breaks) w.Int(b);
      w.Array(static_cast<uint32_t>(ps.col_breaks.size()));
      for (uint32_t b : ps.col_breaks) w.Int(b);
      // element 14 (only when set): {1 Comments, 2 Cell errors as, 3 Draft quality, 4 [[r0, c0, r1, c1]] more print areas,
      // 5 [[section, picture bytes, width, height]] header / footer pictures}; older readers skip it
      if (ps.HasExtras()) {
        uint32_t npics = 0;
        for (const auto& pic : ps.hf_pictures) npics += pic.image.empty() ? 0 : 1;
        w.Map(5);
        w.Int(1);
        w.Int(ps.print_comments);
        w.Int(2);
        w.Int(ps.print_errors);
        w.Int(3);
        w.Bool(ps.draft);
        w.Int(4);
        w.Array(static_cast<uint32_t>(ps.more_areas.size()));
        for (const Rect& a : ps.more_areas) {
          w.Array(4);
          w.Int(a.r0);
          w.Int(a.c0);
          w.Int(a.r1);
          w.Int(a.c1);
        }
        w.Int(5);
        w.Array(npics);
        for (int k = 0; k < 6; ++k) {
          const auto& pic = ps.hf_pictures[k];
          if (pic.image.empty()) continue;
          w.Array(4);
          w.Int(k);
          w.Bin(pic.image);
          w.Double(pic.w);
          w.Double(pic.h);
        }
      }
    }
    // PivotTables: [id, name, source sheet, [r0, c0, r1, c1], source table, [r0, c0], [rows], [columns], [filters], [data],
    //               flag bits, layout, subtotals, style, [has output, r0, c0, r1, c1], [field names]]
    // an axis field: [field, [hidden items], subtotal, descending, page item]; a data field: [field, func, show as, caption, number format]
    w.Int(28);
    w.Array(static_cast<uint32_t>(s.pivots->size()));
    for (const auto& p : *s.pivots) {
      w.Array(22);
      w.Int(p.id);
      w.Str(p.name);
      w.Int(p.src_sheet);
      WriteRect(w, p.src);
      w.Str(p.src_table);
      w.Array(2);
      w.Int(p.r0);
      w.Int(p.c0);
      for (const auto* axis : {&p.rows, &p.cols, &p.pages}) {
        w.Array(static_cast<uint32_t>(axis->size()));
        for (const auto& f : *axis) {
          w.Array(6);
          w.Int(f.field);
          w.Array(static_cast<uint32_t>(f.hidden.size()));
          for (const auto& h : f.hidden) w.Str(h);
          w.Bool(f.subtotal);
          w.Bool(f.descending);
          w.Str(f.page_item);
          WritePivotAxisExt(w, f);  // 5
        }
      }
      w.Array(static_cast<uint32_t>(p.data.size()));
      for (const auto& d : p.data) {
        w.Array(6);
        w.Int(d.field);
        w.Int(static_cast<int>(d.func));
        // older readers know the first five kinds: the others are written in element 5 (here: No Calculation)
        w.Int(static_cast<int>(d.show_as) <= static_cast<int>(model::PivotShowAs::RunningTotal) ? static_cast<int>(d.show_as) : 0);
        w.Str(d.caption);
        w.Str(d.number_format);
        WritePivotDataExt(w, d);  // 5
      }
      w.Int((p.values_on_rows ? 1 : 0) | (p.row_grand ? 2 : 0) | (p.col_grand ? 4 : 0) | (p.from_model ? 8 : 0));  // 8: the Data Model
      w.Int(p.layout);
      w.Int(p.subtotals);
      w.Str(p.style);
      w.Array(5);
      w.Bool(p.has_output);
      w.Int(p.output.r0);
      w.Int(p.output.c0);
      w.Int(p.output.r1);
      w.Int(p.output.c1);
      w.Array(static_cast<uint32_t>(p.fields.size()));
      for (const auto& f : p.fields) w.Str(f);
      // 17: groups [[field, by, start, end, interval]]; 18: calculated fields [[name, formula]]
      w.Array(static_cast<uint32_t>(p.groups.size()));
      for (const auto& g : p.groups) {
        w.Array(6);
        w.Int(g.field);
        // older readers know kinds up to Numbers: a group of items is none for them (element 5 has the kind)
        w.Int(g.by == model::PivotGroupBy::Items ? 0 : static_cast<int>(g.by));
        w.Double(g.start);
        w.Double(g.end);
        w.Double(g.interval);
        WritePivotGroupExt(w, g);  // 5
      }
      w.Array(static_cast<uint32_t>(p.calc_fields.size()));
      for (const auto& c : p.calc_fields) {
        w.Array(2);
        w.Str(c.name);
        w.Str(c.formula);
      }
      // 19: slicer filters [[field, [hidden items]]]
      w.Array(static_cast<uint32_t>(p.slicer_filters.size()));
      for (const auto& f : p.slicer_filters) {
        w.Array(2);
        w.Int(f.field);
        w.Array(static_cast<uint32_t>(f.hidden.size()));
        for (const auto& h : f.hidden) w.Str(h);
      }
      // 20: calculated items [[field, name, formula]]; 21: timeline date filters [[field, from, to]]
      w.Array(static_cast<uint32_t>(p.calc_items.size()));
      for (const auto& c : p.calc_items) {
        w.Array(3);
        w.Int(c.field);
        w.Str(c.name);
        w.Str(c.formula);
      }
      w.Array(static_cast<uint32_t>(p.date_filters.size()));
      for (const auto& f : p.date_filters) {
        w.Array(3);
        w.Int(f.field);
        w.Double(f.from);
        w.Double(f.to);
      }
      WritePivotOptions(w, p, styles);  // 21: options
    }
    // slicers: [id, name, caption, pivot, field, [left, top, width, height], columns, z]
    w.Int(29);
    w.Array(static_cast<uint32_t>(s.slicers->size()));
    for (const auto& sl : *s.slicers) {
      w.Array(14);
      w.Int(sl.id);
      w.Str(sl.name);
      w.Str(sl.caption);
      w.Str(sl.pivot);
      w.Int(sl.field);
      w.Array(4);
      w.Double(sl.left);
      w.Double(sl.top);
      w.Double(sl.width);
      w.Double(sl.height);
      w.Int(sl.columns);
      w.Int(sl.z);
      w.Bool(sl.timeline);  // 9, 10: timelines
      w.Int(sl.level);
      w.Int(model::SlicerFlags(sl));  // 11: Slicer Settings
      WriteStrList(w, sl.more_pivots);  // 12: Report Connections (more PivotTables)
      w.Str(sl.table);                  // 13: a table's slicer
      w.Int(sl.tl_hide);                // 14: timeline Show options (bits hidden)
    }
    // 30 data tables: [[r0, c0, r1, c1], has_row, row_r, row_c, has_col, col_r, col_c]
    w.Int(30);
    w.Array(static_cast<uint32_t>(s.data_tables->size()));
    for (const auto& t : *s.data_tables) {
      w.Array(7);
      WriteRect(w, t.rect);
      w.Bool(t.has_row);
      w.Int(t.row_r);
      w.Int(t.row_c);
      w.Bool(t.has_col);
      w.Int(t.col_r);
      w.Int(t.col_c);
    }
    // 31 scenarios: [name, comment, [[r, c]...], [values], locked, hidden]
    w.Int(31);
    w.Array(static_cast<uint32_t>(s.scenarios->size()));
    for (const auto& scn : *s.scenarios) {
      w.Array(6);
      w.Str(scn.name);
      w.Str(scn.comment);
      w.Array(static_cast<uint32_t>(scn.cells.size()));
      for (const auto& c : scn.cells) {
        w.Array(2);
        w.Int(c.row);
        w.Int(c.col);
      }
      w.Array(static_cast<uint32_t>(scn.values.size()));
      for (const auto& v : scn.values) {
        if (v.IsNumeric()) w.Double(v.AsDouble());
        else if (v.IsBool()) w.Bool(v.boolean());
        else if (v.IsString()) w.Str(std::string(v.str()));
        else w.Nil();
      }
      w.Bool(scn.locked);
      w.Bool(scn.hidden);
    }
    // 32 Solver model: [defined, objective, goal, value, changing, [[ref, rel, rhs]], non_negative, method, max_iterations, precision, max_time]
    w.Int(32);
    {
      const auto& m = *s.solver;
      w.Array(11);
      w.Bool(m.defined);
      w.Str(m.objective);
      w.Int(m.goal);
      w.Double(m.value);
      w.Str(m.changing);
      w.Array(static_cast<uint32_t>(m.constraints.size()));
      for (const auto& c : m.constraints) {
        w.Array(3);
        w.Str(c.ref);
        w.Int(c.rel);
        w.Str(c.rhs);
      }
      w.Bool(m.non_negative);
      w.Int(m.method);
      w.Int(m.max_iterations);
      w.Double(m.precision);
      w.Double(m.max_time);
    }
    // 80 Solver Options {1 automatic scaling, 2 show iteration results, 3 ignore integer constraints, 4 integer optimality
    // (%), 5 max subproblems, 6 max feasible solutions, 7 convergence, 8 derivatives, 9 multistart, 10 require bounds,
    // 11 population size, 12 random seed, 13 mutation rate, 14 maximum time without improvement}
    {
      const auto& m = *s.solver;
      w.Int(80);
      w.Map(14);
      w.KBool(1, m.auto_scaling);
      w.KBool(2, m.show_iterations);
      w.KBool(3, m.ignore_integer);
      w.KDouble(4, m.integer_optimality);
      w.KInt(5, m.max_subproblems);
      w.KInt(6, m.max_feasible);
      w.KDouble(7, m.convergence);
      w.KInt(8, m.derivatives);
      w.KBool(9, m.multistart);
      w.KBool(10, m.require_bounds);
      w.KInt(11, m.population);
      w.KInt(12, m.random_seed);
      w.KDouble(13, m.mutation_rate);
      w.KDouble(14, m.max_time_no_improve);
    }
    // 81 Consolidate: [function, top row, left column, create links, [references]]
    {
      const auto& co = *s.consolidate;
      w.Int(81);
      w.Array(5);
      w.Int(co.func);
      w.Bool(co.top_row);
      w.Bool(co.left_col);
      w.Bool(co.links);
      w.Array(static_cast<uint32_t>(co.refs.size()));
      for (const auto& ref : co.refs) w.Str(ref);
    }
    // 33 rich text: [[row, col, text, [[start, font, size, color, bold, italic, strike, underline, script]]]]
    w.Int(33);
    w.Array(static_cast<uint32_t>(s.rich->size()));
    for (const auto& [k, rt] : *s.rich) {
      w.Array(4);
      w.Int(static_cast<int64_t>(k >> 16));
      w.Int(static_cast<int64_t>(k & 0xFFFF));
      w.Str(rt.text);
      w.Array(static_cast<uint32_t>(rt.runs.size()));
      for (const auto& run : rt.runs) {
        w.Array(9);
        w.Int(run.start);
        w.Str(run.font);
        w.Double(run.size);
        w.Int(run.color);
        w.Int(run.bold);
        w.Int(run.italic);
        w.Int(run.strike);
        w.Int(run.underline);
        w.Int(run.script);
      }
    }
    // 47 Ignore Error (error checking): the cells whose rule is ignored
    w.Int(47);
    WriteRects(w, *s.ignored_errors);
    // 68 the notes shown all the time (Show/Hide Note), as one-cell rectangles; after 13, so the notes are read first
    {
      std::vector<Rect> shown;
      for (const auto& [k, n] : *s.notes) {
        if (n.visible && !n.threaded) shown.push_back(Rect::Cell(static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)));
      }
      w.Int(68);
      WriteRects(w, shown);
    }
    // 71 the tab is selected (grouped sheets)
    w.KBool(71, s.tab_selected);
    // 70 linked pictures (Paste Options > Linked Picture): [[shape id, source sheet id, [rect]]] (counted in the map: always written)
    {
      w.Int(70);
      w.Array(static_cast<uint32_t>(s.linked_pictures->size()));
      for (const auto& lp : *s.linked_pictures) {
        w.Array(3);
        w.Int(lp.shape);
        w.Int(lp.sheet);
        WriteRect(w, lp.rect);
      }
    }
    // 100 Page Layout > Background: the picture tiled behind the cells
    if (!s.background->empty()) {
      w.Int(100);
      w.Bin(*s.background);
    }
    add(kSHET, std::move(w.buf()));
  }
  // EXTL: other workbooks formulas refer to (before FMLA, which refers to their sheets by position)
  // [[path, [sheet names], updated, error, [values per sheet: bin(varint count, then varint row, varint column, value)]]]
  if (!wb.external_links->empty()) {
    MsgWriter w;
    w.Array(static_cast<uint32_t>(wb.external_links->size()));
    for (const auto& l : *wb.external_links) {
      w.Array(5);
      w.Str(l.path);
      w.Array(static_cast<uint32_t>(l.sheets.size()));
      for (const auto& sname : l.sheets) w.Str(sname);
      w.Double(l.updated);
      w.Str(l.error);
      w.Array(static_cast<uint32_t>(l.values.size()));
      for (const auto& m : l.values) {
        std::string b;
        PutVar(b, m.size());
        for (const auto& [k, v] : m) {
          PutVar(b, k >> 16);
          PutVar(b, k & 0xFFFF);
          PutValue(b, v);
        }
        w.Bin(b);
      }
    }
    add(kEXTL, std::move(w.buf()));
  }
  // FMLA
  {
    MsgWriter w;
    w.Map(1);
    w.Int(1);
    w.Array(fmla_count);
    w.Raw(fmla_items);
    add(kFMLA, std::move(w.buf()));
  }
  for (auto& c : cell_chunks) add(kCELL, std::move(c));
  // NAME
  {
    MsgWriter w;
    w.Array(static_cast<uint32_t>(wb.names->size()));
    for (const auto& n : *wb.names) {
      w.Map(6);
      w.KStr(1, n.name);
      w.KStr(2, n.refers_to);
      w.KInt(3, n.sheet_scope);
      w.KStr(4, n.comment);
      w.KBool(5, n.hidden);
      w.KBool(6, true);  // relative references are relative to A1 (older files: they read as absolute)
    }
    add(kNAME, std::move(w.buf()));
  }
  // KVST
  {
    MsgWriter w;
    WriteStringMap(w, *wb.store, true);
    add(kKVST, std::move(w.buf()));
  }
  // PROP
  {
    MsgWriter w;
    w.Map(static_cast<uint32_t>(wb.doc_props->size()));
    for (const auto& [k, p] : *wb.doc_props) {
      w.Str(k);
      w.Map(3);
      w.Int(1);
      WriteMsgValue(w, p.value);
      w.KBool(2, p.link_to_content);
      w.KStr(3, p.link_source);
    }
    add(kPROP, std::move(w.buf()));
  }
  // PANL
  {
    MsgWriter w;
    // UserForms are shown by Python at run time: not saved
    uint32_t saved_panels = 0;
    for (const auto& p : *wb.panels) saved_panels += p.form ? 0 : 1;
    w.Array(saved_panels);
    for (const auto& p : *wb.panels) {
      if (p.form) continue;
      w.Map(9);  // 8, 9: colors (older versions skip the keys)
      w.KStr(1, p.name);
      w.KStr(2, p.caption);
      w.KStr(3, p.dock);
      w.KDouble(4, p.width);
      w.KDouble(5, p.height);
      w.KBool(6, p.visible);
      w.Int(7);
      w.Array(static_cast<uint32_t>(p.controls.size()));
      for (const auto& c : p.controls) {
        // 13-18: styling, 19: the spin's step (older versions skip the keys)
        w.Map(19);
        w.KStr(1, c.type);
        w.KStr(2, c.name);
        w.KStr(3, c.caption);
        w.KStr(4, c.linked_cell);
        w.KStr(5, c.on_action);
        w.KStr(6, c.list_fill_range);
        w.KStr(7, c.chart);
        w.Int(8);
        w.Array(static_cast<uint32_t>(c.items.size()));
        for (const auto& s : c.items) w.Str(s);
        w.KStr(9, c.value);
        w.KDouble(10, c.min);
        w.KDouble(11, c.max);
        w.KBool(12, c.enabled);
        w.KInt(13, c.back_color);
        w.KInt(14, c.fore_color);
        w.KDouble(15, c.font_size);
        w.KBool(16, c.bold);
        w.KStr(17, c.number_format);
        w.KDouble(18, c.height);
        w.KDouble(19, c.small_change);
      }
      w.KInt(8, p.back_color);
      w.KInt(9, p.fore_color);
    }
    add(kPANL, std::move(w.buf()));
  }
  // CONT
  {
    MsgWriter w;
    w.Array(static_cast<uint32_t>(wb.containers->size()));
    for (const auto& c : *wb.containers) {
      w.Map(8);
      w.KStr(1, c.name);
      w.KStr(2, c.caption);
      w.KDouble(3, c.left);
      w.KDouble(4, c.top);
      w.KDouble(5, c.width);
      w.KDouble(6, c.height);
      w.KBool(7, c.floating);
      w.KBool(8, c.created_by_python);
    }
    add(kCONT, std::move(w.buf()));
  }
  // THEM: the document theme {1 name, 2 [12 colors], 3 colors name, 4 major font, 5 minor font, 6 fonts name}
  if (!(*wb.theme == model::DefaultTheme())) {
    const model::Theme& th = *wb.theme;
    MsgWriter w;
    w.Map(6);
    w.KStr(1, th.name);
    w.Int(2);
    w.Array(model::kThemeColors);
    for (uint32_t c : th.colors) w.Int(c);
    w.KStr(3, th.colors_name);
    w.KStr(4, th.major_font);
    w.KStr(5, th.minor_font);
    w.KStr(6, th.fonts_name);
    add(kTHEM, std::move(w.buf()));
  }
  // CSTY: custom cell styles [[name, style map (as in STYL), builtin, includes]]
  if (!wb.cell_styles->empty()) {
    MsgWriter w;
    w.Array(static_cast<uint32_t>(wb.cell_styles->size()));
    for (const auto& ns : *wb.cell_styles) {
      w.Array(4);
      w.Str(ns.name);
      model::Style plain = ns.style;
      plain.cell_style = 0;
      WriteStyle(w, plain, *wb.styles);
      w.Bool(ns.builtin);
      w.Int(ns.includes);
    }
    add(kCSTY, std::move(w.buf()));
  }
  // BIND: [YC] bound tables [{1 name, 2 sheet id, 3 key column, 4 columns [[name, number format, flash, up color,
  //   down color, flash ms, max hz]], 5 stale after (s), 6 show age, 7 recorded (written when false)}]
  if (!wb.bound_tables->empty()) {
    MsgWriter w;
    w.Array(static_cast<uint32_t>(wb.bound_tables->size()));
    for (const auto& b : *wb.bound_tables) {
      w.Map(b.record ? 6 : 7);
      w.KStr(1, b.name);
      w.KInt(2, b.sheet);
      w.KStr(3, b.key);
      w.Int(4);
      w.Array(static_cast<uint32_t>(b.columns.size()));
      for (const auto& c : b.columns) {
        // [name, format, flash, up, down, flash ms, max hz] + [render, render color, render neg, render max, trend len]
        // when the column is drawn (older readers skip the extra fields)
        w.Array(c.render ? 12 : 7);
        w.Str(c.name);
        w.Str(c.number_format);
        w.Int(c.flash);
        w.Int(c.up_color);
        w.Int(c.down_color);
        w.Int(c.flash_ms);
        w.Int(c.max_hz);
        if (c.render) {
          w.Int(c.render);
          w.Int(c.render_color);
          w.Int(c.render_neg);
          w.Double(c.render_max);
          w.Int(c.trend_len);
        }
      }
      w.KDouble(5, b.stale_after);
      w.KBool(6, b.show_age);
      if (!b.record) w.KBool(7, false);
    }
    add(kBIND, std::move(w.buf()));
  }
  // SAFE: [YC] safety {1: parameters [[sheet, row, col, name, type, has min, min, has max, max, [choices], confirm,
  //   max step]], 2: change log [[time, user, kind, target, before, after, source]], 3: settings {1 start read-only,
  //   2 has pin, 3 pin hash, 4 operator idle s, 5 kill macro, 6 host timeout s}}
  if (!wb.parameters->empty() || !wb.audit_log->empty() || !(*wb.safety == model::SafetySettings{}) || !wb.alerts->empty()) {
    MsgWriter w;
    w.Map(4);
    w.Int(1);
    w.Array(static_cast<uint32_t>(wb.parameters->size()));
    for (const auto& pd : *wb.parameters) {
      w.Array(12);
      w.Int(pd.sheet);
      w.Int(pd.row);
      w.Int(pd.col);
      w.Str(pd.name);
      w.Int(pd.type);
      w.Bool(pd.has_min);
      w.Double(pd.min);
      w.Bool(pd.has_max);
      w.Double(pd.max);
      w.Array(static_cast<uint32_t>(pd.choices.size()));
      for (const auto& c : pd.choices) w.Str(c);
      w.Bool(pd.confirm);
      w.Double(pd.max_step);
    }
    w.Int(2);
    w.Array(static_cast<uint32_t>(wb.audit_log->size()));
    for (const auto& a : *wb.audit_log) {
      w.Array(7);
      w.Double(a.time);
      w.Str(a.user);
      w.Str(a.kind);
      w.Str(a.target);
      w.Str(a.before);
      w.Str(a.after);
      w.Str(a.source);
    }
    const auto& st = *wb.safety;
    w.Int(3);
    w.Map(6);
    w.KBool(1, st.start_read_only);
    w.KBool(2, st.has_pin);
    w.KInt(3, st.pin_hash);
    w.KInt(4, st.operator_idle_s);
    w.KStr(5, st.kill_macro);
    w.KDouble(6, st.host_timeout_s);
    // 4: alert rules [[name, sheet, r0, c0, r1, c1, op, a, b, flags (1 sound, 2 popup, 4 flash window, 8 enabled,
    //    16 Windows notification), color, cooldown]]
    w.Int(4);
    w.Array(static_cast<uint32_t>(wb.alerts->size()));
    for (const auto& al : *wb.alerts) {
      w.Array(12);
      w.Str(al.name);
      w.Int(al.sheet);
      w.Int(al.range.r0);
      w.Int(al.range.c0);
      w.Int(al.range.r1);
      w.Int(al.range.c1);
      w.Int(al.op);
      w.Double(al.a);
      w.Double(al.b);
      w.Int((al.sound ? 1 : 0) | (al.popup ? 2 : 0) | (al.flash_window ? 4 : 0) | (al.enabled ? 8 : 0) | (al.notify ? 16 : 0));
      w.Int(al.color);
      w.Double(al.cooldown_s);
    }
    add(kSAFE, std::move(w.buf()));
  }
  // NBOK: [YC] the former Notebook window's cells {1: [code of each cell]}, kept (ADR 0011)
  if (!wb.notebook->empty()) {
    MsgWriter w;
    w.Map(1);
    w.Int(1);
    w.Array(static_cast<uint32_t>(wb.notebook->size()));
    for (const auto& c : *wb.notebook) w.Str(c);
    add(kNBOK, std::move(w.buf()));
  }
  // LAYT
  add(kLAYT, opt.layout_json.empty() ? wb.layout_json : opt.layout_json);
  // chunks we can't read but must keep are written back as is
  for (const auto& u : *wb.unknown_chunks) chunks.push_back(ChunkOut{u.fourcc, u.version, u.flags, u.data});

  // ---- Assembling the file ----
  std::string file(48, '\0');
  struct Dir {
    uint32_t fourcc;
    uint16_t version, flags;
    uint64_t offset, stored, raw;
    uint8_t comp;
    uint32_t crc;
  };
  std::vector<Dir> dir;
  for (const ChunkOut& c : chunks) {
    bool comp = false;
    const std::string stored = opt.compress ? Compress(c.data, &comp) : c.data;
    dir.push_back(Dir{c.fourcc, c.version, c.flags, file.size(), stored.size(), c.data.size(), static_cast<uint8_t>(comp ? 1 : 0),
                      Crc32c(stored.data(), stored.size())});
    file += stored;
  }
  const uint64_t dir_offset = file.size();
  std::string d;
  for (const Dir& e : dir) {
    PutLE(d, e.fourcc, 4);
    PutLE(d, e.version, 2);
    PutLE(d, e.flags, 2);
    PutLE(d, e.offset, 8);
    PutLE(d, e.stored, 8);
    PutLE(d, e.raw, 8);
    PutLE(d, e.comp, 1);
    PutLE(d, 0, 3);
    PutLE(d, e.crc, 4);
  }
  PutLE(d, Crc32c(d.data(), d.size()), 4);
  file += d;
  std::string h;
  h += "YCG1";
  PutLE(h, kFormatMajor, 2);
  PutLE(h, kFormatMinor, 2);
  PutLE(h, 1, 2);  // min_reader_major
  PutLE(h, 0, 2);  // min_reader_minor
  PutLE(h, 0, 8);  // required_features
  PutLE(h, 0, 8);  // optional_features
  PutLE(h, chunks.size(), 4);
  PutLE(h, dir_offset, 8);
  PutLE(h, 0, 4);
  PutLE(h, Crc32c(h.data(), h.size()), 4);
  std::memcpy(file.data(), h.data(), 48);
  return file;
}

LoadResult LoadYcgFromBytes(std::string_view bytes, const std::string& password) {
  if (IsEncryptedYcg(bytes)) {
    LoadResult er;
    std::string plain;
    if (password.empty()) {
      er.needs_password = true;
      er.error = TR("This workbook is protected with a password.");
      return er;
    }
    if (!DecryptYcg(bytes, password, &plain)) {
      er.wrong_password = true;
      er.error = TR("The password you supplied is not correct.");
      return er;
    }
    LoadResult r = LoadYcgFromBytes(plain);
    if (r.ok) r.wb.open_password = password;
    return r;
  }
  LoadResult res;
  model::Workbook& wb = res.wb;
  wb.sheets.clear();
  if (bytes.size() < 48 || bytes.substr(0, 4) != "YCG1") {
    res.error = TR("This isn't a YCGrid workbook file (.ycg).");
    return res;
  }
  if (Crc32c(bytes.data(), 44) != static_cast<uint32_t>(GetLE(bytes, 44, 4))) {
    res.error = TR("The file header is damaged.");
    return res;
  }
  const uint16_t fmaj = static_cast<uint16_t>(GetLE(bytes, 4, 2)), fmin = static_cast<uint16_t>(GetLE(bytes, 6, 2));
  const uint16_t rmaj = static_cast<uint16_t>(GetLE(bytes, 8, 2));
  const uint64_t required = GetLE(bytes, 12, 8);
  const uint32_t count = static_cast<uint32_t>(GetLE(bytes, 28, 4));
  const uint64_t dir_offset = GetLE(bytes, 32, 8);
  wb.file_format_major = fmaj;
  wb.file_format_minor = fmin;
  if (rmaj > kFormatMajor || required != 0) {
    wb.read_only = true;
    wb.read_only_reason = TR("This file was created by a newer YCGrid; some content needs the newer version to edit. Opened read-only.");
  }
  if (dir_offset > bytes.size() || uint64_t{count} * 40 + 4 > bytes.size() - dir_offset) {  // no wrapping sums
    res.error = TR("The file directory is damaged (the file may be incomplete).");
    return res;
  }
  const std::string_view dirb = bytes.substr(static_cast<size_t>(dir_offset), size_t{count} * 40);
  if (Crc32c(dirb.data(), dirb.size()) != static_cast<uint32_t>(GetLE(bytes, static_cast<size_t>(dir_offset) + size_t{count} * 40, 4))) {
    res.error = TR("The file directory checksum doesn't match (the file may be damaged).");
    return res;
  }
  struct Entry {
    uint32_t fourcc;
    uint16_t version, flags;
    std::string data;
  };
  std::vector<Entry> entries;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = size_t{i} * 40;
    Entry e;
    e.fourcc = static_cast<uint32_t>(GetLE(dirb, at, 4));
    e.version = static_cast<uint16_t>(GetLE(dirb, at + 4, 2));
    e.flags = static_cast<uint16_t>(GetLE(dirb, at + 6, 2));
    const uint64_t off = GetLE(dirb, at + 8, 8), stored = GetLE(dirb, at + 16, 8), raw = GetLE(dirb, at + 24, 8);
    const uint8_t comp = static_cast<uint8_t>(GetLE(dirb, at + 32, 1));
    const uint32_t crc = static_cast<uint32_t>(GetLE(dirb, at + 36, 4));
    if (off > dir_offset || stored > dir_offset - off) {
      res.error = "chunk " + FourCCText(e.fourcc) + TR(" is outside the file.");
      return res;
    }
    const std::string_view sv = bytes.substr(static_cast<size_t>(off), static_cast<size_t>(stored));
    if (Crc32c(sv.data(), sv.size()) != crc) {
      if (e.flags & kChunkCritical) {
        res.error = "chunk " + FourCCText(e.fourcc) + TR(" has a checksum mismatch (the file is damaged).");
        return res;
      }
      res.warnings.push_back(TR("Skipped damaged chunk ") + FourCCText(e.fourcc));
      continue;
    }
    if (comp == 1) {
      if (!Decompress(sv, raw, &e.data)) {
        res.error = TR("Couldn't decompress chunk ") + FourCCText(e.fourcc) + TR(".");
        return res;
      }
    } else {
      e.data.assign(sv);
    }
    entries.push_back(std::move(e));
  }

  // unknown or newer chunk version
  std::vector<model::RawChunk> unknown;
  auto usable = [&](const Entry& e) {
    const KnownChunk* k = Known(e.fourcc);
    if (k && e.version <= k->version) return true;
    if (e.flags & kChunkCritical) {
      wb.read_only = true;
      wb.read_only_reason = TR("The file has required content this version can't read (") + FourCCText(e.fourcc) + " v" + std::to_string(e.version) + TR("); opened read-only. Please update YCGrid.");
    }
    if (e.flags & kChunkPreserve) unknown.push_back(model::RawChunk{e.fourcc, e.version, e.flags, e.data});
    else res.warnings.push_back(TR("Skipped unknown content ") + FourCCText(e.fourcc));
    return false;
  };

  auto fail = [&](const std::string& what) {
    res.error = what;
    return res;
  };
  std::vector<uint32_t> order;
  std::map<uint32_t, model::Sheet> sheets;
  std::vector<uint32_t> style_ids;  // file index -> new style table id
  auto& styles = wb.styles.mut();
  // processed in dependency order: WKBK -> STYL -> SHET -> FMLA -> CELL -> others
  for (const Entry& e : entries) {
    if (e.fourcc != kWKBK || !usable(e)) continue;
    MsgReader r(e.data);
    uint32_t n;
    int64_t dialect = 0;  // absent in files saved before the formula dialect existed
    if (!r.Map(&n)) return fail(TR("WKBK is malformed."));
    for (uint32_t i = 0; i < n; ++i) {
      int64_t k;
      if (!r.Int(&k)) return fail(TR("WKBK is malformed."));
      bool ok = true;
      switch (k) {
        case 1: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t j = 0; ok && j < m; ++j) {
            int64_t id;
            ok = r.Int(&id);
            order.push_back(static_cast<uint32_t>(id));
          }
          break;
        }
        case 2: ok = r.IntAs(&wb.active_sheet); break;
        case 3: ok = r.IntAs(&wb.next_sheet_id); break;
        case 4: ok = r.IntAs(&wb.next_object_id); break;
        case 6: ok = r.IntAs(&dialect); break;
        case 5: {
          uint32_t m;
          int64_t h = 0;
          ok = r.Array(&m) && m >= 3 && r.Bool(&wb.protect_structure) && r.Bool(&wb.protect_windows) && r.Int(&h);
          for (uint32_t q = 3; ok && q < m; ++q) ok = r.Skip();
          wb.protect_hash = static_cast<uint16_t>(h);
          break;
        }
        case 7: {
          uint32_t m;
          bool on = false;
          int64_t it = 100;
          double ch = 0.001;
          int64_t mode = -1;
          ok = r.Array(&m) && m >= 3 && r.Bool(&on) && r.Int(&it) && r.Double(&ch) && (m < 4 || r.Int(&mode));
          for (uint32_t q = 4; ok && q < m; ++q) ok = r.Skip();
          wb.calc_mode = static_cast<int8_t>(mode >= 0 && mode <= 2 ? mode : -1);
          wb.calc_iterate = on ? 1 : 0;
          wb.calc_max_iterations = static_cast<int32_t>(std::clamp<int64_t>(it, 1, 32767));
          wb.calc_max_change = ch > 0 ? ch : 0.001;
          break;
        }
        case 8: {
          uint32_t m;
          ok = r.Array(&m) && m >= 3 && r.Bool(&wb.date1904) && r.Bool(&wb.precision_as_displayed) && r.Bool(&wb.force_full_calc);
          for (uint32_t q = 3; ok && q < m; ++q) ok = r.Skip();
          break;
        }
        default: ok = r.Skip(); break;
      }
      if (!ok) return fail(TR("WKBK is malformed."));
    }
    wb.legacy_decimal = dialect < 1;
  }
  for (const Entry& e : entries) {
    if (e.fourcc != kSTYL || !usable(e)) continue;
    MsgReader r(e.data);
    uint32_t n;
    if (!r.Map(&n)) return fail(TR("STYL is malformed."));
    for (uint32_t i = 0; i < n; ++i) {
      int64_t k;
      if (!r.Int(&k)) return fail(TR("STYL is malformed."));
      bool ok = true;
      switch (k) {
        case 1: ok = r.Str(&styles.default_font); break;
        case 2: {
          double v;
          ok = r.Double(&v);
          styles.default_font_size = static_cast<float>(v);
          break;
        }
        case 3: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t j = 0; ok && j < m; ++j) {
            model::Style st;
            ok = ReadStyle(r, &st, styles);
            // the default again after the first: a cell's own Normal under its row's or column's formats
            style_ids.push_back(j > 0 && st == model::Style{} ? model::kStyleOwnNormal : styles.Intern(st));
          }
          break;
        }
        default: ok = r.Skip(); break;
      }
      if (!ok) return fail(TR("STYL is malformed."));
    }
  }
  auto style_of = [&](uint64_t idx) -> uint32_t { return idx < style_ids.size() ? style_ids[static_cast<size_t>(idx)] : 0; };
  for (const Entry& e : entries) {
    if (e.fourcc != kSHET || !usable(e)) continue;
    MsgReader r(e.data);
    uint32_t n;
    if (!r.Map(&n)) return fail(TR("SHET is malformed."));
    Sheet s;
    for (uint32_t i = 0; i < n; ++i) {
      int64_t k;
      if (!r.Int(&k)) return fail(TR("SHET is malformed."));
      bool ok = true;
      switch (k) {
        case 1: ok = r.IntAs(&s.id); break;
        case 2: ok = r.Str(&s.name); break;
        case 3: ok = r.Str(&s.code_name); break;
        case 4: ok = r.IntAs(&s.row_limit); break;
        case 5: ok = r.IntAs(&s.col_limit); break;
        case 6: ok = r.Bool(&s.visible); break;
        case 72: ok = r.Bool(&s.very_hidden); break;
        case 34: ok = r.Bool(&s.chart_sheet); break;
        case 35: {
          int64_t f = 0;
          ok = r.Int(&f);
          s.outline_below = !(f & 1);
          s.outline_right = !(f & 2);
          s.outline_styles = (f & 4) != 0;
          break;
        }
        case 7: ok = r.IntAs(&s.tab_color); break;
        case 8: ok = ReadDims(r, &s.rows.mut()); break;
        case 9: ok = ReadDims(r, &s.cols.mut()); break;
        case 10: {
          uint32_t m;
          ok = r.Array(&m);
          auto& mg = s.merges.mut();
          for (uint32_t j = 0; ok && j < m; ++j) {
            Rect rc;
            ok = ReadRect(r, &rc);
            if (ok && rc.r0 <= rc.r1 && rc.c0 <= rc.c1 && rc.r1 < core::kMaxRows && rc.c1 < core::kMaxCols) mg.push_back(rc);
          }
          break;
        }
        case 11:
        case 12: {
          uint32_t m;
          ok = r.Map(&m);
          auto& mp = k == 11 ? s.row_styles.mut() : s.col_styles.mut();
          for (uint32_t j = 0; ok && j < m; ++j) {
            int64_t a = 0, b = 0;
            ok = r.Int(&a) && r.Int(&b);
            mp[static_cast<uint32_t>(a)] = style_of(static_cast<uint64_t>(b));
          }
          break;
        }
        case 13: {
          uint32_t m;
          ok = r.Array(&m);
          auto& notes = s.notes.mut();
          for (uint32_t j = 0; ok && j < m; ++j) {
            uint32_t four = 0;
            int64_t rr = 0, cc = 0;
            model::CellNote note;
            ok = r.Array(&four) && four >= 4 && r.Int(&rr) && r.Int(&cc) && r.Str(&note.author) && r.Str(&note.text);
            if (ok && four >= 8) {
              uint32_t nr = 0;
              ok = r.Bool(&note.threaded) && r.Double(&note.date) && r.Bool(&note.resolved) && r.Array(&nr);
              for (uint32_t q = 0; ok && q < nr; ++q) {
                uint32_t f = 0;
                model::NoteReply rp;
                ok = r.Array(&f) && f >= 3 && r.Str(&rp.author) && r.Str(&rp.text) && r.Double(&rp.date);
                for (uint32_t z = 3; ok && z < f; ++z) ok = r.Skip();
                note.replies.push_back(std::move(rp));
              }
            }
            if (ok && four >= 9) {
              uint32_t nm = 0;
              ok = r.Map(&nm);
              auto& lk = note.look;
              for (uint32_t q = 0; ok && q < nm; ++q) {
                int64_t key = 0;
                double dv = 0;
                ok = r.Int(&key);
                if (!ok) break;
                switch (key) {
                  case 1: ok = r.Double(&lk.w); break;
                  case 2: ok = r.Double(&lk.h); break;
                  case 3: ok = r.Double(&lk.dx); break;
                  case 4: ok = r.Double(&lk.dy); break;
                  case 5: ok = r.Str(&lk.font_name); break;
                  case 6: ok = r.Double(&dv), lk.font_size = static_cast<float>(dv); break;
                  case 7: ok = r.Bool(&lk.bold); break;
                  case 8: ok = r.Bool(&lk.italic); break;
                  case 9: ok = r.IntAs(&lk.font_color); break;
                  case 10: ok = r.IntAs(&lk.fill_color); break;
                  case 11: ok = r.IntAs(&lk.line_color); break;
                  case 12: ok = r.IntAs(&lk.halign); break;
                  case 13: ok = r.IntAs(&lk.valign); break;
                  default: ok = r.Skip(); break;
                }
              }
            }
            for (uint32_t q = four >= 9 ? 9 : four >= 8 ? 8 : 4; ok && q < four; ++q) ok = r.Skip();
            notes[(static_cast<uint64_t>(rr) << 16) | static_cast<uint64_t>(cc)] = note;
          }
          break;
        }
        case 14: ok = ReadStringMap(r, &s.store.mut(), true); break;
        case 100: {  // Page Layout > Background
          std::string bg;
          ok = r.Bin(&bg);
          if (ok) s.background.reset(std::move(bg));
          break;
        }
        case 71: ok = r.Bool(&s.tab_selected); break;  // grouped sheets
        case 70: {  // linked pictures
          uint32_t lp_count = 0;
          ok = r.Array(&lp_count);
          auto& lps = s.linked_pictures.mut();
          for (uint32_t li = 0; ok && li < lp_count; ++li) {
            uint32_t fields = 0;
            int64_t lp_shape = 0, lp_src = 0;
            model::Sheet::LinkedPicture lp;
            ok = r.Array(&fields) && fields >= 3 && r.Int(&lp_shape) && r.Int(&lp_src) && ReadRect(r, &lp.rect);
            for (uint32_t fk = 3; ok && fk < fields; ++fk) r.Skip();
            lp.shape = static_cast<uint32_t>(lp_shape);
            lp.sheet = static_cast<uint32_t>(lp_src);
            if (ok && lp.rect.r0 <= lp.rect.r1 && lp.rect.c0 <= lp.rect.c1 && lp.rect.r1 < core::kMaxRows && lp.rect.c1 < core::kMaxCols) lps.push_back(lp);
          }
          break;
        }
        case 68: {  // notes shown all the time
          std::vector<Rect> v;
          ok = ReadRects(r, &v);
          for (const Rect& x : v) {
            auto it = s.notes->find((static_cast<uint64_t>(x.r0) << 16) | x.c0);
            if (it != s.notes->end() && !it->second.threaded) s.notes.mut()[it->first].visible = true;
          }
          break;
        }
        case 47: {  // Ignore Error
          std::vector<Rect> v;
          ok = ReadRects(r, &v);
          auto& ig = s.ignored_errors.mut();
          for (const Rect& x : v) {
            if (x.r0 <= x.r1 && x.c0 <= x.c1 && x.r1 < core::kMaxRows && x.c1 < core::kMaxCols) ig.push_back(x);
          }
          break;
        }
        case 33: {
          uint32_t m = 0;
          ok = r.Array(&m);
          auto& rich = s.rich.mut();
          for (uint32_t j = 0; ok && j < m; ++j) {
            uint32_t f = 0, nr = 0;
            int64_t rr = 0, cc = 0;
            model::RichText rt;
            ok = r.Array(&f) && f >= 4 && r.Int(&rr) && r.Int(&cc) && r.Str(&rt.text) && r.Array(&nr);
            for (uint32_t q = 0; ok && q < nr; ++q) {
              uint32_t g = 0;
              model::RichRun run;
              int64_t b = 0, it = 0, st = 0, un = 0, sc = 0;
              double size = 0;
              ok = r.Array(&g) && g >= 9 && r.IntAs(&run.start) && r.Str(&run.font) && r.Double(&size) && r.IntAs(&run.color) && r.Int(&b) &&
                   r.Int(&it) && r.Int(&st) && r.Int(&un) && r.Int(&sc);
              run.size = static_cast<float>(size);
              run.bold = static_cast<int8_t>(b);
              run.italic = static_cast<int8_t>(it);
              run.strike = static_cast<int8_t>(st);
              run.underline = static_cast<int8_t>(un);
              run.script = static_cast<int8_t>(sc);
              for (uint32_t z = 9; ok && z < g; ++z) ok = r.Skip();
              rt.runs.push_back(std::move(run));
            }
            for (uint32_t z = 4; ok && z < f; ++z) ok = r.Skip();
            if (ok) rich[(static_cast<uint64_t>(rr) << 16) | static_cast<uint64_t>(cc)] = std::move(rt);
          }
          break;
        }
        case 15: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t j = 0; ok && j < m; ++j) {
            model::Control c;
            ok = ReadControl(r, &c);
            s.controls.mut().push_back(std::move(c));
          }
          break;
        }
        case 16: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t j = 0; ok && j < m; ++j) {
            model::ChartObj c;
            ok = ReadChart(r, &c);
            s.charts.mut().push_back(std::move(c));
          }
          break;
        }
        case 17: {
          uint32_t m;
          ok = r.Array(&m) && m >= 6 && r.IntAs(&s.view.top_row) && r.IntAs(&s.view.left_col) && r.IntAs(&s.view.act_row) && r.IntAs(&s.view.act_col) &&
               r.IntAs(&s.view.freeze_row) && r.IntAs(&s.view.freeze_col);
          for (uint32_t q = 6; ok && q < m; ++q) ok = r.Skip();
          break;
        }
        case 18: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            model::CfRule c;
            ok = ReadCf(r, &c);
            if (!c.fmt.numfmt.empty()) styles.InternString(c.fmt.numfmt);  // the rule's number format is drawn by its id
            s.cond_formats.mut().push_back(std::move(c));
          }
          break;
        }
        case 19: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            model::DvRule d;
            ok = ReadDv(r, &d);
            s.validations.mut().push_back(std::move(d));
          }
          break;
        }
        case 20: {
          model::AutoFilter a;
          ok = ReadAf(r, &a);
          s.autofilter.mut() = std::move(a);
          break;
        }
        case 21: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            uint32_t f;
            int64_t row = 0, col = 0;
            model::Hyperlink l;
            ok = r.Array(&f) && f >= 5 && r.Int(&row) && r.Int(&col) && r.Str(&l.address) && r.Str(&l.sub_address) && r.Str(&l.tip);
            for (uint32_t z = 5; ok && z < f; ++z) ok = r.Skip();
            if (ok) s.links.mut()[(static_cast<uint64_t>(row) << 16) | static_cast<uint64_t>(col)] = std::move(l);
          }
          break;
        }
        case 22: {
          uint32_t m;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            uint32_t f;
            int64_t row = 0, col = 0, rows = 0, cols = 0;
            bool cse = false;
            ok = r.Array(&f) && f >= 5 && r.Int(&row) && r.Int(&col) && r.Int(&rows) && r.Int(&cols) && r.Bool(&cse);
            for (uint32_t z = 5; ok && z < f; ++z) ok = r.Skip();
            if (!ok || row < 0 || col < 0 || rows < 1 || cols < 1 || row + rows > int64_t{core::kMaxRows} || col + cols > int64_t{core::kMaxCols}) continue;
            model::SpillRange sp;
            sp.rect = Rect{static_cast<uint32_t>(row), static_cast<uint32_t>(col), static_cast<uint32_t>(row + rows - 1), static_cast<uint32_t>(col + cols - 1)};
            sp.cse = cse;
            s.spills.mut()[(static_cast<uint64_t>(row) << 16) | static_cast<uint64_t>(col)] = sp;
          }
          break;
        }
        case 23: {
          uint32_t m = 0, nf = 0, nr = 0;
          model::SheetProtection pr;
          int64_t hash = 0;
          ok = r.Array(&m) && m >= 4 && r.Bool(&pr.on) && r.Int(&hash) && r.Array(&nf);
          bool* flags[] = {&pr.select_locked, &pr.select_unlocked, &pr.format_cells, &pr.format_columns, &pr.format_rows, &pr.insert_columns, &pr.insert_rows,
                           &pr.insert_hyperlinks, &pr.delete_columns, &pr.delete_rows, &pr.sort, &pr.autofilter, &pr.pivot, &pr.objects, &pr.scenarios};
          for (uint32_t q = 0; ok && q < nf; ++q) {
            bool b = false;
            ok = r.Bool(&b);
            if (ok && q < std::size(flags)) *flags[q] = b;
          }
          ok = ok && r.Array(&nr);
          for (uint32_t q = 0; ok && q < nr; ++q) {
            uint32_t f;
            model::AllowEditRange er;
            int64_t r0 = 0, c0 = 0, r1 = 0, c1 = 0, h = 0;
            ok = r.Array(&f) && f >= 6 && r.Str(&er.title) && r.Int(&r0) && r.Int(&c0) && r.Int(&r1) && r.Int(&c1) && r.Int(&h);
            if (ok && f >= 7) {
              uint32_t nu = 0;
              ok = r.Array(&nu);
              for (uint32_t z = 0; ok && z < nu; ++z) {
                uint32_t two = 0;
                model::AllowEditRange::User u;
                ok = r.Array(&two) && two >= 2 && r.Str(&u.name) && r.Bool(&u.allow);
                for (uint32_t y2 = 2; ok && y2 < two; ++y2) ok = r.Skip();
                er.users.push_back(std::move(u));
              }
            }
            for (uint32_t z = 7; ok && z < f; ++z) ok = r.Skip();
            er.rect = Rect{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(r1), static_cast<uint32_t>(c1)};
            er.hash = static_cast<uint16_t>(h);
            if (ok) pr.ranges.push_back(std::move(er));
          }
          for (uint32_t q = 4; ok && q < m; ++q) ok = r.Skip();
          pr.hash = static_cast<uint16_t>(hash);
          if (ok) s.protection.mut() = std::move(pr);
          break;
        }
        case 24: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            uint32_t f = 0, nc = 0;
            model::Table t;
            int64_t flags = 0;
            ok = r.Array(&f) && f >= 6 && r.IntAs(&t.id) && r.Str(&t.name) && ReadRect(r, &t.range) && r.Str(&t.style) && r.Int(&flags) && r.Array(&nc);
            for (uint32_t z = 0; ok && z < nc; ++z) {
              uint32_t g = 0;
              model::TableColumn c;
              int64_t fn = 0;
              ok = r.Array(&g) && g >= 3 && r.Str(&c.name) && r.Int(&fn) && r.Str(&c.totals_label);
              for (uint32_t y = 3; ok && y < g; ++y) ok = r.Skip();
              c.totals_func = static_cast<int>(fn);
              t.columns.push_back(std::move(c));
            }
            if (ok && f > 6) ok = ReadAf(r, &t.filter);
            for (uint32_t z = 7; ok && z < f; ++z) ok = r.Skip();
            t.header = flags & 1;
            t.totals = flags & 2;
            t.banded_rows = flags & 4;
            t.banded_cols = flags & 8;
            t.first_col = flags & 16;
            t.last_col = flags & 32;
            t.filter_button = flags & 64;
            if (ok) s.tables.mut().push_back(std::move(t));
          }
          break;
        }
        case 25: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            uint32_t f = 0, a = 0, b = 0, c = 0;
            model::ShapeObj sh;
            int64_t kind = 0, pl = 0, fc = 0, lc = 0, tc = 0, ha = 1, va = 1;
            double lw = 0.75, fs = 11;
            ok = r.Array(&f) && f >= 11 && r.IntAs(&sh.id) && r.Int(&kind) && r.Str(&sh.name) && r.Array(&a) && a >= 4 && r.Double(&sh.left) &&
                 r.Double(&sh.top) && r.Double(&sh.width) && r.Double(&sh.height);
            for (uint32_t z = 4; ok && z < a; ++z) ok = r.Skip();
            ok = ok && r.Int(&pl) && r.Array(&b) && b >= 7 && r.Bool(&sh.fill_visible) && r.Bool(&sh.line_visible) && r.Int(&fc) && r.Int(&lc) &&
                 r.Double(&lw) && r.Bool(&sh.flip_h) && r.Bool(&sh.flip_v);
            for (uint32_t z = 7; ok && z < b; ++z) ok = r.Skip();
            ok = ok && r.Array(&c) && c >= 8 && r.Str(&sh.text) && r.Str(&sh.font_name) && r.Double(&fs) && r.Int(&tc) && r.Bool(&sh.bold) &&
                 r.Bool(&sh.italic) && r.Int(&ha) && r.Int(&va);
            for (uint32_t z = 8; ok && z < c; ++z) ok = r.Skip();
            ok = ok && r.Bin(&sh.image) && r.Str(&sh.on_action) && r.Bool(&sh.visible) && r.IntAs(&sh.z);
            // versions before 2026-10 wrote a header of 12 for these 11 elements: nothing follows them in those files
            // (and a picture of those files keeps its proportions, like a new one)
            if (f == 11 || f == 12) sh.lock_aspect = static_cast<model::ShapeKind>(kind) == model::ShapeKind::Picture;
            if (ok && f >= 13) ok = ReadShapeExtra(r, &sh);
            for (uint32_t z = (f >= 13 ? 12u : 11u); ok && z < (f == 12 ? 11u : f); ++z) ok = r.Skip();
            sh.kind = static_cast<model::ShapeKind>(kind);
            sh.placement = static_cast<model::Placement>(pl);
            sh.fill_color = static_cast<uint32_t>(fc);
            sh.line_color = static_cast<uint32_t>(lc);
            sh.font_color = static_cast<uint32_t>(tc);
            sh.line_weight = static_cast<float>(lw);
            sh.font_size = static_cast<float>(fs);
            sh.halign = static_cast<int>(ha);
            sh.valign = static_cast<int>(va);
            if (ok) s.shapes.mut().push_back(std::move(sh));
          }
          break;
        }
        case 26: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            uint32_t f = 0, ni = 0, nc = 0;
            model::SparklineGroup g;
            int64_t flags = 0;
            double lw = 0.75;
            ok = r.Array(&f) && f >= 6 && r.IntAs(&g.id) && r.IntAs(&g.type) && r.Array(&ni);
            for (uint32_t z = 0; ok && z < ni; ++z) {
              uint32_t kk = 0;
              model::Sparkline it;
              ok = r.Array(&kk) && kk >= 7 && r.IntAs(&it.r) && r.IntAs(&it.c) && r.IntAs(&it.src_sheet) && r.IntAs(&it.src.r0) && r.IntAs(&it.src.c0) &&
                   r.IntAs(&it.src.r1) && r.IntAs(&it.src.c1);
              for (uint32_t y = 7; ok && y < kk; ++y) ok = r.Skip();
              g.items.push_back(it);
            }
            ok = ok && r.Array(&nc);
            uint32_t* cols[] = {&g.color, &g.negative_color, &g.marker_color, &g.high_color, &g.low_color, &g.first_color, &g.last_color};
            for (uint32_t z = 0; ok && z < nc; ++z) {
              int64_t v = 0;
              ok = r.Int(&v);
              if (ok && z < std::size(cols)) *cols[z] = static_cast<uint32_t>(v);
            }
            ok = ok && r.Int(&flags) && r.Double(&lw);
            if (ok && f >= 7) {
              uint32_t nm = 0;
              ok = r.Map(&nm);
              for (uint32_t z = 0; ok && z < nm; ++z) {
                int64_t key = 0;
                ok = r.Int(&key);
                if (!ok) break;
                switch (key) {
                  case 1: ok = r.IntAs(&g.display_blanks); break;
                  case 2: ok = r.Bool(&g.show_hidden); break;
                  case 3: ok = r.IntAs(&g.min_scale); break;
                  case 4: ok = r.IntAs(&g.max_scale); break;
                  case 5: ok = r.Double(&g.min_value); break;
                  case 6: ok = r.Double(&g.max_value); break;
                  case 7: ok = r.Bool(&g.right_to_left); break;
                  case 8: ok = r.Str(&g.date_range); break;
                  case 9: ok = r.IntAs(&g.axis_color); break;
                  default: ok = r.Skip(); break;
                }
              }
            }
            for (uint32_t z = 7; ok && z < f; ++z) ok = r.Skip();
            g.high = flags & 1;
            g.low = flags & 2;
            g.first = flags & 4;
            g.last = flags & 8;
            g.negative = flags & 16;
            g.markers = flags & 32;
            g.axis = flags & 64;
            g.line_weight = static_cast<float>(lw);
            if (ok) s.sparklines.mut().push_back(std::move(g));
          }
          break;
        }
        case 27: {
          model::PageSetup ps;
          uint32_t f = 0, pn = 0;
          int64_t flags = 0;
          ok = r.Array(&f) && f >= 14 && r.Bool(&ps.landscape) && r.IntAs(&ps.paper) && r.Array(&pn);
          double* margins[] = {&ps.left, &ps.right, &ps.top, &ps.bottom, &ps.header, &ps.footer};
          for (uint32_t z = 0; ok && z < pn; ++z) {
            double v = 0;
            ok = r.Double(&v);
            if (ok && z < std::size(margins)) *margins[z] = v;
          }
          ok = ok && r.IntAs(&ps.zoom) && r.IntAs(&ps.fit_wide) && r.IntAs(&ps.fit_tall);
          ok = ok && r.Array(&pn) && pn >= 5 && r.Bool(&ps.has_area) && r.IntAs(&ps.area.r0) && r.IntAs(&ps.area.c0) && r.IntAs(&ps.area.r1) &&
               r.IntAs(&ps.area.c1);
          for (uint32_t z = 5; ok && z < pn; ++z) ok = r.Skip();
          ok = ok && r.Array(&pn) && pn >= 3 && r.Bool(&ps.has_title_rows) && r.IntAs(&ps.title_r0) && r.IntAs(&ps.title_r1);
          for (uint32_t z = 3; ok && z < pn; ++z) ok = r.Skip();
          ok = ok && r.Array(&pn) && pn >= 3 && r.Bool(&ps.has_title_cols) && r.IntAs(&ps.title_c0) && r.IntAs(&ps.title_c1);
          for (uint32_t z = 3; ok && z < pn; ++z) ok = r.Skip();
          ok = ok && r.Int(&flags) && r.IntAs(&ps.first_page_number) && r.Array(&pn);
          std::string* texts[] = {&ps.header_left, &ps.header_center, &ps.header_right, &ps.footer_left, &ps.footer_center, &ps.footer_right};
          for (uint32_t z = 0; ok && z < pn; ++z) {
            std::string t;
            ok = r.Str(&t);
            if (ok && z < std::size(texts)) *texts[z] = std::move(t);
            else if (ok && z < 12) ps.first_hf[z - 6] = std::move(t);
            else if (ok && z < 18) ps.even_hf[z - 12] = std::move(t);
          }
          for (std::vector<uint32_t>* br : {&ps.row_breaks, &ps.col_breaks}) {
            ok = ok && r.Array(&pn);
            for (uint32_t z = 0; ok && z < pn; ++z) {
              uint32_t b = 0;
              ok = r.IntAs(&b);
              br->push_back(b);
            }
          }
          if (ok && f > 14) {
            uint32_t m = 0;
            ok = r.Map(&m);
            for (uint32_t q = 0; ok && q < m; ++q) {
              int64_t key = 0;
              ok = r.Int(&key);
              if (!ok) break;
              if (key == 1) ok = r.IntAs(&ps.print_comments);
              else if (key == 2) ok = r.IntAs(&ps.print_errors);
              else if (key == 3) ok = r.Bool(&ps.draft);
              else if (key == 4) {
                uint32_t na = 0;
                ok = r.Array(&na);
                for (uint32_t z = 0; ok && z < na; ++z) {
                  uint32_t ne = 0;
                  Rect a;
                  ok = r.Array(&ne) && ne >= 4 && r.IntAs(&a.r0) && r.IntAs(&a.c0) && r.IntAs(&a.r1) && r.IntAs(&a.c1);
                  for (uint32_t y = 4; ok && y < ne; ++y) ok = r.Skip();
                  if (ok) ps.more_areas.push_back(a);
                }
              } else if (key == 5) {
                uint32_t np = 0;
                ok = r.Array(&np);
                for (uint32_t z = 0; ok && z < np; ++z) {
                  uint32_t ne = 0;
                  int64_t sec = 0;
                  model::PageSetup::HfPicture pic;
                  ok = r.Array(&ne) && ne >= 4 && r.Int(&sec) && r.Bin(&pic.image) && r.Double(&pic.w) && r.Double(&pic.h);
                  for (uint32_t y = 4; ok && y < ne; ++y) ok = r.Skip();
                  if (ok && sec >= 0 && sec < 6) ps.hf_pictures[sec] = std::move(pic);
                }
              } else {
                ok = r.Skip();
              }
            }
          }
          for (uint32_t z = 15; ok && z < f; ++z) ok = r.Skip();
          ps.gridlines = flags & 1;
          ps.headings = flags & 2;
          ps.center_h = flags & 4;
          ps.center_v = flags & 8;
          ps.black_and_white = flags & 16;
          ps.over_then_down = flags & 32;
          ps.different_first = flags & 64;
          ps.different_odd_even = flags & 128;
          ps.no_scale_with_doc = flags & 256;
          ps.no_align_margins = flags & 512;
          if (ok) s.page_setup.mut() = std::move(ps);
          break;
        }
        case 30: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            model::DataTable t;
            uint32_t f = 0;
            ok = r.Array(&f) && f >= 7 && ReadRect(r, &t.rect) && r.Bool(&t.has_row) && r.IntAs(&t.row_r) && r.IntAs(&t.row_c) && r.Bool(&t.has_col) &&
                 r.IntAs(&t.col_r) && r.IntAs(&t.col_c);
            for (uint32_t z = 7; ok && z < f; ++z) ok = r.Skip();
            if (ok) s.data_tables.mut().push_back(t);
          }
          break;
        }
        case 31: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            model::Scenario sc;
            uint32_t f = 0, cnt = 0;
            ok = r.Array(&f) && f >= 6 && r.Str(&sc.name) && r.Str(&sc.comment) && r.Array(&cnt);
            for (uint32_t z = 0; ok && z < cnt; ++z) {
              uint32_t two = 0;
              core::CellPos p{};
              ok = r.Array(&two) && two >= 2 && r.IntAs(&p.row) && r.IntAs(&p.col);
              for (uint32_t y = 2; ok && y < two; ++y) ok = r.Skip();
              sc.cells.push_back(p);
            }
            ok = ok && r.Array(&cnt);
            for (uint32_t z = 0; ok && z < cnt; ++z) {
              MV v;
              ok = Decode(r, &v);
              if (v.t == MV::T::Double || v.t == MV::T::Int) sc.values.push_back(core::Value::Number(v.Num()));
              else if (v.t == MV::T::Bool) sc.values.push_back(core::Value::Bool(v.b));
              else if (v.t == MV::T::Str) sc.values.push_back(core::Value::String(v.s));
              else sc.values.push_back(core::Value());
            }
            ok = ok && r.Bool(&sc.locked) && r.Bool(&sc.hidden);
            for (uint32_t z = 6; ok && z < f; ++z) ok = r.Skip();
            if (ok) s.scenarios.mut().push_back(std::move(sc));
          }
          break;
        }
        case 80: {  // Solver Options (read after 32 or before: kept in the model either way)
          uint32_t f = 0;
          ok = r.Map(&f);
          model::SolverModel& m = s.solver.mut();
          for (uint32_t z = 0; ok && z < f; ++z) {
            int64_t key = 0;
            ok = r.Int(&key);
            if (!ok) break;
            switch (key) {
              case 1: ok = r.Bool(&m.auto_scaling); break;
              case 2: ok = r.Bool(&m.show_iterations); break;
              case 3: ok = r.Bool(&m.ignore_integer); break;
              case 4: ok = r.Double(&m.integer_optimality); break;
              case 5: ok = r.IntAs(&m.max_subproblems); break;
              case 6: ok = r.IntAs(&m.max_feasible); break;
              case 7: ok = r.Double(&m.convergence); break;
              case 8: ok = r.IntAs(&m.derivatives); break;
              case 9: ok = r.Bool(&m.multistart); break;
              case 10: ok = r.Bool(&m.require_bounds); break;
              case 11: ok = r.IntAs(&m.population); break;
              case 12: ok = r.IntAs(&m.random_seed); break;
              case 13: ok = r.Double(&m.mutation_rate); break;
              case 14: ok = r.Double(&m.max_time_no_improve); break;
              default: ok = r.Skip(); break;
            }
          }
          break;
        }
        case 81: {  // Consolidate settings
          model::ConsolidateSettings co;
          uint32_t f = 0, cnt = 0;
          ok = r.Array(&f) && f >= 5 && r.IntAs(&co.func) && r.Bool(&co.top_row) && r.Bool(&co.left_col) && r.Bool(&co.links) && r.Array(&cnt);
          for (uint32_t z = 0; ok && z < cnt; ++z) {
            std::string ref;
            ok = r.Str(&ref);
            co.refs.push_back(std::move(ref));
          }
          for (uint32_t z = 5; ok && z < f; ++z) ok = r.Skip();
          if (ok) s.consolidate.mut() = std::move(co);
          break;
        }
        case 32: {
          model::SolverModel sm;
          uint32_t f = 0, cnt = 0;
          ok = r.Array(&f) && f >= 11 && r.Bool(&sm.defined) && r.Str(&sm.objective) && r.IntAs(&sm.goal) && r.Double(&sm.value) && r.Str(&sm.changing) &&
               r.Array(&cnt);
          for (uint32_t z = 0; ok && z < cnt; ++z) {
            model::SolverConstraint c;
            uint32_t three = 0;
            ok = r.Array(&three) && three >= 3 && r.Str(&c.ref) && r.IntAs(&c.rel) && r.Str(&c.rhs);
            for (uint32_t y = 3; ok && y < three; ++y) ok = r.Skip();
            sm.constraints.push_back(std::move(c));
          }
          ok = ok && r.Bool(&sm.non_negative) && r.IntAs(&sm.method) && r.IntAs(&sm.max_iterations) && r.Double(&sm.precision) && r.Double(&sm.max_time);
          for (uint32_t z = 11; ok && z < f; ++z) ok = r.Skip();
          if (ok) {
            // the Options (key 80) may have been read first: keep them
            const model::SolverModel& was = *s.solver;
            sm.auto_scaling = was.auto_scaling, sm.show_iterations = was.show_iterations, sm.ignore_integer = was.ignore_integer;
            sm.integer_optimality = was.integer_optimality, sm.max_subproblems = was.max_subproblems, sm.max_feasible = was.max_feasible;
            sm.convergence = was.convergence, sm.derivatives = was.derivatives, sm.multistart = was.multistart, sm.require_bounds = was.require_bounds;
            sm.population = was.population, sm.random_seed = was.random_seed, sm.mutation_rate = was.mutation_rate;
            sm.max_time_no_improve = was.max_time_no_improve;
            s.solver.mut() = std::move(sm);
          }
          break;
        }
        case 29: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            model::Slicer sl;
            uint32_t f = 0, g = 0;
            ok = r.Array(&f) && f >= 8 && r.IntAs(&sl.id) && r.Str(&sl.name) && r.Str(&sl.caption) && r.Str(&sl.pivot) && r.IntAs(&sl.field) && r.Array(&g) &&
                 g >= 4 && r.Double(&sl.left) && r.Double(&sl.top) && r.Double(&sl.width) && r.Double(&sl.height);
            for (uint32_t z = 4; ok && z < g; ++z) ok = r.Skip();
            ok = ok && r.IntAs(&sl.columns) && r.IntAs(&sl.z);
            if (ok && f >= 10) ok = r.Bool(&sl.timeline) && r.IntAs(&sl.level);
            if (ok && f >= 11) {
              int flags = 0;
              ok = r.IntAs(&flags);
              model::SetSlicerFlags(sl, flags);
            }
            if (ok && f >= 12) ok = ReadStrList(r, &sl.more_pivots);
            if (ok && f >= 13) ok = r.Str(&sl.table);
            if (ok && f >= 14) ok = r.IntAs(&sl.tl_hide);
            for (uint32_t z = 14; ok && z < f; ++z) ok = r.Skip();
            if (ok) s.slicers.mut().push_back(std::move(sl));
          }
          break;
        }
        case 28: {
          uint32_t m = 0;
          ok = r.Array(&m);
          for (uint32_t q = 0; ok && q < m; ++q) {
            model::PivotTable p;
            p.general_values = false;  // files before options key 22: values keep the source's number format
            uint32_t f = 0, pn = 0;
            int64_t flags = 0;
            ok = r.Array(&f) && f >= 16 && r.IntAs(&p.id) && r.Str(&p.name) && r.IntAs(&p.src_sheet) && ReadRect(r, &p.src) && r.Str(&p.src_table) &&
                 r.Array(&pn) && pn >= 2 && r.IntAs(&p.r0) && r.IntAs(&p.c0);
            for (uint32_t z = 2; ok && z < pn; ++z) ok = r.Skip();
            for (auto* axis : {&p.rows, &p.cols, &p.pages}) {
              ok = ok && r.Array(&pn);
              for (uint32_t z = 0; ok && z < pn; ++z) {
                model::PivotAxisField af;
                uint32_t pk = 0, nh = 0;
                ok = r.Array(&pk) && pk >= 5 && r.IntAs(&af.field) && r.Array(&nh);
                for (uint32_t y = 0; ok && y < nh; ++y) {
                  std::string h;
                  ok = r.Str(&h);
                  af.hidden.push_back(std::move(h));
                }
                ok = ok && r.Bool(&af.subtotal) && r.Bool(&af.descending) && r.Str(&af.page_item);
                if (ok && pk >= 6) ok = ReadPivotAxisExt(r, &af);
                for (uint32_t y = 6; ok && y < pk; ++y) ok = r.Skip();
                axis->push_back(std::move(af));
              }
            }
            ok = ok && r.Array(&pn);
            for (uint32_t z = 0; ok && z < pn; ++z) {
              model::PivotDataField df;
              uint32_t pk = 0;
              int64_t fn = 0, sa = 0;
              ok = r.Array(&pk) && pk >= 5 && r.IntAs(&df.field) && r.Int(&fn) && r.Int(&sa) && r.Str(&df.caption) && r.Str(&df.number_format);
              df.func = static_cast<model::PivotFunc>(std::clamp<int64_t>(fn, 0, model::kPivotFuncs - 1));
              df.show_as = static_cast<model::PivotShowAs>(std::clamp<int64_t>(sa, 0, 4));
              if (ok && pk >= 6) ok = ReadPivotDataExt(r, &df);
              for (uint32_t y = 6; ok && y < pk; ++y) ok = r.Skip();
              p.data.push_back(std::move(df));
            }
            ok = ok && r.Int(&flags) && r.IntAs(&p.layout) && r.IntAs(&p.subtotals) && r.Str(&p.style) && r.Array(&pn) && pn >= 5 && r.Bool(&p.has_output) &&
                 r.IntAs(&p.output.r0) && r.IntAs(&p.output.c0) && r.IntAs(&p.output.r1) && r.IntAs(&p.output.c1);
            for (uint32_t z = 5; ok && z < pn; ++z) ok = r.Skip();
            ok = ok && r.Array(&pn);
            for (uint32_t z = 0; ok && z < pn; ++z) {
              std::string fname;
              ok = r.Str(&fname);
              p.fields.push_back(std::move(fname));
            }
            if (ok && f >= 18) {
              ok = r.Array(&pn);
              for (uint32_t z = 0; ok && z < pn; ++z) {
                model::PivotGroup g;
                uint32_t pk = 0;
                int64_t by = 0;
                ok = r.Array(&pk) && pk >= 5 && r.IntAs(&g.field) && r.Int(&by) && r.Double(&g.start) && r.Double(&g.end) && r.Double(&g.interval);
                g.by = static_cast<model::PivotGroupBy>(std::clamp<int64_t>(by, 0, 6));
                if (ok && pk >= 6) ok = ReadPivotGroupExt(r, &g);
                for (uint32_t y = 6; ok && y < pk; ++y) ok = r.Skip();
                p.groups.push_back(g);
              }
              ok = ok && r.Array(&pn);
              for (uint32_t z = 0; ok && z < pn; ++z) {
                model::PivotCalcField c;
                uint32_t pk = 0;
                ok = r.Array(&pk) && pk >= 2 && r.Str(&c.name) && r.Str(&c.formula);
                for (uint32_t y = 2; ok && y < pk; ++y) ok = r.Skip();
                p.calc_fields.push_back(std::move(c));
              }
            }
            if (ok && f >= 19) {
              ok = r.Array(&pn);
              for (uint32_t z = 0; ok && z < pn; ++z) {
                model::PivotAxisField sf;
                uint32_t pk = 0, nh = 0;
                ok = r.Array(&pk) && pk >= 2 && r.IntAs(&sf.field) && r.Array(&nh);
                for (uint32_t y = 0; ok && y < nh; ++y) {
                  std::string h;
                  ok = r.Str(&h);
                  sf.hidden.push_back(std::move(h));
                }
                for (uint32_t y = 2; ok && y < pk; ++y) ok = r.Skip();
                p.slicer_filters.push_back(std::move(sf));
              }
            }
            if (ok && f >= 20) {
              ok = r.Array(&pn);
              for (uint32_t z = 0; ok && z < pn; ++z) {
                model::PivotCalcItem c;
                uint32_t pk = 0;
                ok = r.Array(&pk) && pk >= 3 && r.IntAs(&c.field) && r.Str(&c.name) && r.Str(&c.formula);
                for (uint32_t y = 3; ok && y < pk; ++y) ok = r.Skip();
                p.calc_items.push_back(std::move(c));
              }
            }
            if (ok && f >= 21) {
              ok = r.Array(&pn);
              for (uint32_t z = 0; ok && z < pn; ++z) {
                model::PivotDateFilter df;
                uint32_t pk = 0;
                ok = r.Array(&pk) && pk >= 3 && r.IntAs(&df.field) && r.Double(&df.from) && r.Double(&df.to);
                for (uint32_t y = 3; ok && y < pk; ++y) ok = r.Skip();
                p.date_filters.push_back(df);
              }
            }
            if (ok && f >= 22) ok = ReadPivotOptions(r, &p, styles);
            for (uint32_t z = 22; ok && z < f; ++z) ok = r.Skip();
            // a slicer and the field's own filter are one filter: a file from before keeps its slicer's items hidden
            for (auto it = p.slicer_filters.begin(); ok && it != p.slicer_filters.end();) {
              model::PivotAxisField* on = nullptr;
              for (auto* axis : {&p.rows, &p.cols, &p.pages}) {
                for (auto& a : *axis) {
                  if (a.field == it->field) on = &a;
                }
              }
              if (!on) {
                ++it;
                continue;
              }
              for (const auto& h : it->hidden) {
                if (std::find(on->hidden.begin(), on->hidden.end(), h) == on->hidden.end()) on->hidden.push_back(h);
              }
              it = p.slicer_filters.erase(it);
            }
            p.values_on_rows = flags & 1;
            p.from_model = (flags & 8) != 0;
            p.row_grand = flags & 2;
            p.col_grand = flags & 4;
            if (ok) s.pivots.mut().push_back(std::move(p));
          }
          break;
        }
        default: ok = r.Skip(); break;
      }
      if (!ok) return fail(TR("Sheet data is malformed."));
    }
    s.row_limit = std::clamp<uint32_t>(s.row_limit, 1, core::kMaxRows);
    s.col_limit = std::clamp<uint32_t>(s.col_limit, 1, core::kMaxCols);
    const uint32_t id = s.id;
    sheets.emplace(id, std::move(s));
  }
  // sheets in WKBK order; those not listed in WKBK go last
  for (uint32_t id : order) {
    auto it = sheets.find(id);
    if (it == sheets.end()) continue;
    wb.sheets.emplace_back(std::make_shared<Sheet>(std::move(it->second)));
    sheets.erase(it);
  }
  for (auto& [id, s] : sheets) wb.sheets.emplace_back(std::make_shared<Sheet>(std::move(s)));
  if (wb.sheets.empty()) return fail(TR("The file contains no sheets."));
  for (const auto& s : wb.sheets) wb.next_sheet_id = std::max(wb.next_sheet_id, s->id + 1);
  wb.active_sheet = std::clamp<int32_t>(wb.active_sheet, 0, static_cast<int32_t>(wb.sheets.size()) - 1);

  // EXTL: the links come first, so the formulas' references to other workbooks find their cached values
  for (const Entry& e : entries) {
    if (e.fourcc != kEXTL || !usable(e)) continue;
    MsgReader r(e.data);
    uint32_t n = 0;
    if (!r.Array(&n)) continue;
    auto& links = wb.external_links.mut();
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t f = 0, ns = 0, nv = 0;
      model::ExternalLink l;
      if (!r.Array(&f) || f < 5 || !r.Str(&l.path) || !r.Array(&ns)) break;
      bool ok = true;
      for (uint32_t j = 0; ok && j < ns; ++j) {
        std::string sname;
        ok = r.Str(&sname);
        l.sheets.push_back(std::move(sname));
      }
      ok = ok && r.Double(&l.updated) && r.Str(&l.error) && r.Array(&nv);
      for (uint32_t j = 0; ok && j < nv; ++j) {
        std::string b;
        ok = r.Bin(&b);
        std::map<uint64_t, Value> m;
        size_t pos = 0;
        uint64_t nvals = 0;
        if (ok && GetVar(b, &pos, &nvals)) {
          for (uint64_t q = 0; q < nvals; ++q) {
            uint64_t rr = 0, cc = 0;
            Value v;
            if (!GetVar(b, &pos, &rr) || !GetVar(b, &pos, &cc) || !GetValue(b, &pos, &v)) break;
            m[(rr << 16) | cc] = std::move(v);
          }
        }
        l.values.push_back(std::move(m));
      }
      for (uint32_t z = 5; ok && z < f; ++z) ok = r.Skip();
      if (!ok) break;
      l.values.resize(l.sheets.size());
      links.push_back(std::move(l));
    }
  }
  // FMLA: parse the A1 text at the origin (sheet names are known)
  std::vector<uint32_t> fmla_ids{0};
  {
    engine::WorkbookResolver res2(wb, true);
    std::unordered_map<std::string, uint32_t> dedup;
    for (const Entry& e : entries) {
      if (e.fourcc != kFMLA || !usable(e)) continue;
      MsgReader r(e.data);
      uint32_t n;
      if (!r.Map(&n)) return fail(TR("FMLA is malformed."));
      for (uint32_t i = 0; i < n; ++i) {
        int64_t k;
        if (!r.Int(&k)) return fail(TR("FMLA is malformed."));
        if (k != 1) {
          r.Skip();
          continue;
        }
        uint32_t m;
        if (!r.Array(&m)) return fail(TR("FMLA is malformed."));
        for (uint32_t j = 0; j < m; ++j) {
          uint32_t four;
          int64_t sid, rr, cc;
          std::string text;
          if (!r.Array(&four) || four < 4 || !r.Int(&sid) || !r.Int(&rr) || !r.Int(&cc) || !r.Str(&text)) return fail(TR("FMLA is malformed."));
          for (uint32_t q = 4; q < four; ++q) r.Skip();
          formula::ParseResult pr = formula::Parse(text, core::CellPos{static_cast<uint32_t>(rr), static_cast<uint32_t>(cc)}, &res2);
          if (!pr.ok) {
            res.warnings.push_back(TR("Couldn't parse the formula: =") + text + TR(" (") + pr.error + TR(")"));
            pr = formula::Parse("#N/A", core::CellPos{0, 0}, &res2);
          }
          std::string key = formula::SerializeKey(pr.root);
          auto it = dedup.find(key);
          if (it == dedup.end()) {
            auto def = std::make_shared<formula::CompiledFormula>();
            def->root = std::move(pr.root);
            def->flags = formula::ComputeFlags(def->root);
            def->key = key;
            const uint32_t id = wb.formulas.mut().Append(def);
            it = dedup.emplace(std::move(key), id).first;
          }
          fmla_ids.push_back(it->second);
        }
      }
    }
  }
  // CELL
  for (const Entry& e : entries) {
    if (e.fourcc != kCELL || !usable(e)) continue;
    std::string_view b = e.data;
    size_t p = 0;
    uint64_t sid, n;
    if (!GetVar(b, &p, &sid) || !GetVar(b, &p, &n)) return fail(TR("Cell data is malformed."));
    Sheet* s = wb.MutSheetById(static_cast<uint32_t>(sid));
    if (!s) {
      res.warnings.push_back(TR("Cell data belongs to a sheet that doesn't exist"));
      continue;
    }
    uint64_t row = 0;
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t dr, c;
      if (!GetVar(b, &p, &dr) || !GetVar(b, &p, &c) || p >= b.size()) return fail(TR("Cell data is malformed."));
      row += dr;
      const uint8_t flags = static_cast<uint8_t>(b[p++]);
      if (c >= core::kMaxCols || row >= core::kMaxRows) return fail(TR("A cell position is out of range."));  // before narrowing
      const uint32_t r = static_cast<uint32_t>(row), col = static_cast<uint32_t>(c);
      if (r >= s->row_limit) s->row_limit = std::min<uint32_t>(core::kMaxRows, (r / core::kDefaultRows + 1) * core::kDefaultRows);
      if (flags & 1) {
        Value v;
        if (!GetValue(b, &p, &v)) return fail(TR("A cell value is malformed."));
        s->SetValue(r, col, std::move(v));
      }
      if (flags & 2) {
        uint64_t fi;
        if (!GetVar(b, &p, &fi)) return fail(TR("A cell formula is malformed."));
        if (fi < fmla_ids.size()) s->SetFormula(r, col, fmla_ids[static_cast<size_t>(fi)]);
      }
      if (flags & 4) {
        uint64_t si;
        if (!GetVar(b, &p, &si)) return fail(TR("A cell style is malformed."));
        s->SetStyle(r, col, style_of(si));
      }
      // a newer version may append data in other flag bits: without length information it can't be skipped, so new fields need a new CELL version
    }
    s->ComputeUsedRange();
  }
  // others
  for (const Entry& e : entries) {
    if (e.fourcc == kWKBK || e.fourcc == kSTYL || e.fourcc == kSHET || e.fourcc == kFMLA || e.fourcc == kCELL) continue;
    if (!usable(e)) continue;
    MsgReader r(e.data);
    if (e.fourcc == kNAME) {
      uint32_t n;
      if (!r.Array(&n)) return fail(TR("NAME is malformed."));
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t m;
        model::NameDef nd;
        bool a1_based = false;
        if (!r.Map(&m)) return fail(TR("NAME is malformed."));
        for (uint32_t j = 0; j < m; ++j) {
          int64_t k;
          bool ok = r.Int(&k);
          if (ok) {
            switch (k) {
              case 1: ok = r.Str(&nd.name); break;
              case 2: ok = r.Str(&nd.refers_to); break;
              case 3: ok = r.IntAs(&nd.sheet_scope); break;
              case 4: ok = r.Str(&nd.comment); break;
              case 5: ok = r.Bool(&nd.hidden); break;
              case 6: ok = r.Bool(&a1_based); break;
              default: ok = r.Skip(); break;
            }
          }
          if (!ok) return fail(TR("NAME is malformed."));
        }
        if (!a1_based) {
          // saved before names kept relative references: they worked as absolute ones and still do
          engine::WorkbookResolver name_sheets(wb);
          nd.refers_to = formula::AbsoluteRefText(nd.refers_to, &name_sheets);
        }
        wb.names.mut().push_back(nd);
      }
    } else if (e.fourcc == kKVST) {
      if (!ReadStringMap(r, &wb.store.mut(), true)) return fail(TR("KV is malformed."));
    } else if (e.fourcc == kPROP) {
      uint32_t n;
      if (!r.Map(&n)) return fail(TR("PROP is malformed."));
      for (uint32_t i = 0; i < n; ++i) {
        std::string name;
        uint32_t m;
        model::DocProp dp;
        if (!r.Str(&name) || !r.Map(&m)) return fail(TR("PROP is malformed."));
        for (uint32_t j = 0; j < m; ++j) {
          int64_t k;
          bool ok = r.Int(&k);
          if (ok) {
            switch (k) {
              case 1: ok = ReadMsgValue(r, &dp.value); break;
              case 2: ok = r.Bool(&dp.link_to_content); break;
              case 3: ok = r.Str(&dp.link_source); break;
              default: ok = r.Skip(); break;
            }
          }
          if (!ok) return fail(TR("PROP is malformed."));
        }
        wb.doc_props.mut()[name] = dp;
      }
    } else if (e.fourcc == kPANL) {
      uint32_t n;
      if (!r.Array(&n)) continue;
      for (uint32_t i = 0; i < n; ++i) {
        model::Panel p;
        uint32_t m;
        if (!r.Map(&m)) break;
        for (uint32_t j = 0; j < m; ++j) {
          int64_t k;
          if (!r.Int(&k)) break;
          switch (k) {
            case 1: r.Str(&p.name); break;
            case 2: r.Str(&p.caption); break;
            case 3: r.Str(&p.dock); break;
            case 4: r.Double(&p.width); break;
            case 5: r.Double(&p.height); break;
            case 6: r.Bool(&p.visible); break;
            case 7: {
              uint32_t q = 0;  // a failed read leaves the counts at 0 (MsgReader doesn't write them)
              r.Array(&q);
              for (uint32_t z = 0; z < q; ++z) {
                model::PanelControl c;
                uint32_t f = 0;
                r.Map(&f);
                for (uint32_t y = 0; y < f; ++y) {
                  int64_t ck = 0;
                  r.Int(&ck);
                  switch (ck) {
                    case 1: r.Str(&c.type); break;
                    case 2: r.Str(&c.name); break;
                    case 3: r.Str(&c.caption); break;
                    case 4: r.Str(&c.linked_cell); break;
                    case 5: r.Str(&c.on_action); break;
                    case 6: r.Str(&c.list_fill_range); break;
                    case 7: r.Str(&c.chart); break;
                    case 8: {
                      uint32_t it = 0;
                      r.Array(&it);
                      for (uint32_t x = 0; x < it; ++x) {
                        std::string sv;
                        r.Str(&sv);
                        c.items.push_back(sv);
                      }
                      break;
                    }
                    case 9: r.Str(&c.value); break;
                    case 10: r.Double(&c.min); break;
                    case 11: r.Double(&c.max); break;
                    case 12: r.Bool(&c.enabled); break;
                    case 13: r.IntAs(&c.back_color); break;
                    case 14: r.IntAs(&c.fore_color); break;
                    case 15: r.Double(&c.font_size); break;
                    case 16: r.Bool(&c.bold); break;
                    case 17: r.Str(&c.number_format); break;
                    case 18: r.Double(&c.height); break;
                    case 19: r.Double(&c.small_change); break;
                    default: r.Skip(); break;
                  }
                }
                p.controls.push_back(std::move(c));
              }
              break;
            }
            case 8: r.IntAs(&p.back_color); break;
            case 9: r.IntAs(&p.fore_color); break;
            default: r.Skip(); break;
          }
        }
        wb.panels.mut().push_back(std::move(p));
      }
    } else if (e.fourcc == kCONT) {
      uint32_t n;
      if (!r.Array(&n)) continue;
      for (uint32_t i = 0; i < n; ++i) {
        model::ContainerDef c;
        uint32_t m;
        if (!r.Map(&m)) break;
        for (uint32_t j = 0; j < m; ++j) {
          int64_t k;
          if (!r.Int(&k)) break;
          switch (k) {
            case 1: r.Str(&c.name); break;
            case 2: r.Str(&c.caption); break;
            case 3: r.Double(&c.left); break;
            case 4: r.Double(&c.top); break;
            case 5: r.Double(&c.width); break;
            case 6: r.Double(&c.height); break;
            case 7: r.Bool(&c.floating); break;
            case 8: r.Bool(&c.created_by_python); break;
            default: r.Skip(); break;
          }
        }
        wb.containers.mut().push_back(std::move(c));
      }
    } else if (e.fourcc == kLAYT) {
      wb.layout_json = e.data;
    } else if (e.fourcc == kTHEM) {
      uint32_t n = 0;
      if (!r.Map(&n)) continue;
      model::Theme th = model::DefaultTheme();
      for (uint32_t j = 0; j < n; ++j) {
        int64_t k;
        if (!r.Int(&k)) break;
        switch (k) {
          case 1: r.Str(&th.name); break;
          case 2: {
            uint32_t m = 0;
            r.Array(&m);
            for (uint32_t z = 0; z < m; ++z) {
              uint32_t c = 0;
              r.IntAs(&c);
              if (z < model::kThemeColors) th.colors[z] = c & 0xFFFFFF;
            }
            break;
          }
          case 3: r.Str(&th.colors_name); break;
          case 4: r.Str(&th.major_font); break;
          case 5: r.Str(&th.minor_font); break;
          case 6: r.Str(&th.fonts_name); break;
          default: r.Skip(); break;
        }
      }
      wb.theme.reset(std::move(th));
    } else if (e.fourcc == kNBOK) {
      uint32_t m = 0;
      if (!r.Map(&m)) continue;
      for (uint32_t j = 0; j < m; ++j) {
        int64_t k = 0;
        if (!r.Int(&k)) break;
        if (k == 1) {
          uint32_t n = 0;
          if (!r.Array(&n)) break;
          auto& cells = wb.notebook.mut();
          for (uint32_t i = 0; i < n; ++i) {
            std::string c;
            if (!r.Str(&c)) break;
            cells.push_back(std::move(c));
          }
        } else {
          r.Skip();
        }
      }
    } else if (e.fourcc == kSAFE) {
      uint32_t m = 0;
      if (!r.Map(&m)) continue;
      for (uint32_t j = 0; j < m; ++j) {
        int64_t k = 0;
        if (!r.Int(&k)) break;
        if (k == 1 || k == 2) {
          uint32_t n = 0;
          if (!r.Array(&n)) break;
          for (uint32_t i = 0; i < n; ++i) {
            uint32_t f = 0;
            if (!r.Array(&f)) break;
            if (k == 1) {
              model::ParameterDef pd;
              int64_t x = 0;
              uint32_t z = 0;
              auto next = [&](auto&& read) {
                if (z < f) read();
                ++z;
              };
              next([&] { r.Int(&x), pd.sheet = static_cast<uint32_t>(x); });
              next([&] { r.Int(&x), pd.row = static_cast<uint32_t>(x); });
              next([&] { r.Int(&x), pd.col = static_cast<uint32_t>(x); });
              next([&] { r.Str(&pd.name); });
              next([&] { r.Int(&x), pd.type = static_cast<int>(x); });
              next([&] { r.Bool(&pd.has_min); });
              next([&] { r.Double(&pd.min); });
              next([&] { r.Bool(&pd.has_max); });
              next([&] { r.Double(&pd.max); });
              next([&] {
                uint32_t nc = 0;
                if (r.Array(&nc)) {
                  for (uint32_t c = 0; c < nc; ++c) {
                    std::string t;
                    r.Str(&t);
                    pd.choices.push_back(std::move(t));
                  }
                }
              });
              next([&] { r.Bool(&pd.confirm); });
              next([&] { r.Double(&pd.max_step); });
              for (; z < f; ++z) r.Skip();
              wb.parameters.mut().push_back(std::move(pd));
            } else {
              model::AuditEntry a;
              std::string* strs[] = {&a.user, &a.kind, &a.target, &a.before, &a.after, &a.source};
              if (f >= 1) r.Double(&a.time);
              for (uint32_t z = 1; z < f; ++z) {
                if (z <= 6) r.Str(strs[z - 1]);
                else r.Skip();
              }
              wb.audit_log.mut().push_back(std::move(a));
            }
          }
        } else if (k == 3) {
          uint32_t fm = 0;
          if (!r.Map(&fm)) break;
          model::SafetySettings st;
          for (uint32_t z = 0; z < fm; ++z) {
            int64_t fk = 0, x = 0;
            if (!r.Int(&fk)) break;
            switch (fk) {
              case 1: r.Bool(&st.start_read_only); break;
              case 2: r.Bool(&st.has_pin); break;
              case 3: r.Int(&x), st.pin_hash = static_cast<uint32_t>(x); break;
              case 4: r.Int(&x), st.operator_idle_s = static_cast<int>(x); break;
              case 5: r.Str(&st.kill_macro); break;
              case 6: r.Double(&st.host_timeout_s); break;
              default: r.Skip(); break;
            }
          }
          wb.safety.mut() = st;
        } else if (k == 4) {
          uint32_t n = 0;
          if (!r.Array(&n)) break;
          for (uint32_t i = 0; i < n; ++i) {
            uint32_t f = 0;
            if (!r.Array(&f) || f < 12) {
              for (uint32_t z = 0; z < f; ++z) r.Skip();
              continue;
            }
            model::AlertRule al;
            int64_t x[6] = {};
            r.Str(&al.name);
            for (int z = 0; z < 6; ++z) r.Int(&x[z]);
            al.sheet = static_cast<uint32_t>(x[0]);
            al.range = core::Rect{static_cast<uint32_t>(x[1]), static_cast<uint32_t>(x[2]), static_cast<uint32_t>(x[3]), static_cast<uint32_t>(x[4])};
            al.op = static_cast<int>(x[5]);
            r.Double(&al.a);
            r.Double(&al.b);
            int64_t flags = 0, color = 0;
            r.Int(&flags);
            r.Int(&color);
            r.Double(&al.cooldown_s);
            al.sound = flags & 1;
            al.popup = (flags & 2) != 0;
            al.flash_window = (flags & 4) != 0;
            al.enabled = (flags & 8) != 0;
            al.notify = (flags & 16) != 0;
            al.color = static_cast<uint32_t>(color);
            for (uint32_t z = 12; z < f; ++z) r.Skip();
            wb.alerts.mut().push_back(std::move(al));
          }
        } else {
          r.Skip();
        }
      }
    } else if (e.fourcc == kBIND) {
      uint32_t n = 0;
      if (!r.Array(&n)) continue;
      auto& v = wb.bound_tables.mut();
      for (uint32_t i = 0; i < n; ++i) {
        model::BoundTable b;
        uint32_t m = 0;
        if (!r.Map(&m)) break;
        for (uint32_t j = 0; j < m; ++j) {
          int64_t k = 0;
          if (!r.Int(&k)) break;
          int64_t x = 0;
          switch (k) {
            case 1: r.Str(&b.name); break;
            case 2:
              r.Int(&x);
              b.sheet = static_cast<uint32_t>(x);
              break;
            case 3: r.Str(&b.key); break;
            case 4: {
              uint32_t nc = 0;
              if (!r.Array(&nc)) break;
              for (uint32_t c = 0; c < nc; ++c) {
                model::BoundColumn col;
                uint32_t f = 0;
                if (!r.Array(&f) || f < 2 || !r.Str(&col.name) || !r.Str(&col.number_format)) break;
                int64_t vals[5] = {col.flash, col.up_color, col.down_color, col.flash_ms, col.max_hz};
                int64_t extra[4] = {0, 0, 0, col.trend_len};  // render, color, neg, trend length
                for (uint32_t z = 2; z < f; ++z) {
                  if (z < 7) r.Int(&vals[z - 2]);
                  else if (z == 7 || z == 8 || z == 9) r.Int(&extra[z - 7]);
                  else if (z == 10) r.Double(&col.render_max);
                  else if (z == 11) r.Int(&extra[3]);
                  else r.Skip();
                }
                col.render = static_cast<int>(extra[0]);
                col.render_color = static_cast<uint32_t>(extra[1]);
                col.render_neg = static_cast<uint32_t>(extra[2]);
                col.trend_len = static_cast<int>(extra[3]);
                col.flash = static_cast<int>(vals[0]);
                col.up_color = static_cast<uint32_t>(vals[1]);
                col.down_color = static_cast<uint32_t>(vals[2]);
                col.flash_ms = static_cast<int>(vals[3]);
                col.max_hz = static_cast<int>(vals[4]);
                b.columns.push_back(std::move(col));
              }
              break;
            }
            case 5: r.Double(&b.stale_after); break;
            case 6: r.Bool(&b.show_age); break;
            case 7: r.Bool(&b.record); break;
            default: r.Skip(); break;
          }
        }
        v.push_back(std::move(b));
      }
    } else if (e.fourcc == kCSTY) {
      uint32_t n = 0;
      if (!r.Array(&n)) continue;
      auto& v = wb.cell_styles.mut();
      for (uint32_t j = 0; j < n; ++j) {
        uint32_t f = 0;
        model::NamedStyle ns;
        if (!r.Array(&f) || f < 3 || !r.Str(&ns.name) || !ReadStyle(r, &ns.style, wb.styles.mut()) || !r.Bool(&ns.builtin)) break;
        if (f >= 4) r.IntAs(&ns.includes);
        for (uint32_t z = 4; z < f; ++z) r.Skip();
        v.push_back(std::move(ns));
      }
    }
  }
  wb.unknown_chunks.reset(std::move(unknown));
  wb.legacy_decimal = false;  // formulas typed from now on use Excel's DECIMAL
  res.upgraded = fmaj < kFormatMajor || (fmaj == kFormatMajor && fmin < kFormatMinor);
  res.ok = true;
  return res;
}

// ---- File ----

bool ReadFileBytes(const std::wstring& path, std::string* out) {
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const auto n = f.tellg();
  if (n < 0) return false;
  f.seekg(0);
  out->resize(static_cast<size_t>(n));
  f.read(out->data(), n);
  return static_cast<bool>(f);
}

bool WriteFileAtomic(const std::wstring& path, std::string_view data, std::string* error) {
  const std::wstring tmp = path + L".tmp~";
  const std::filesystem::path target(path);
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) {
      // why, as Excel says it (one message for every cause sent people looking for a read-only folder)
      const std::wstring name = target.filename().wstring();
      std::error_code ec;
      if (name.find_first_of(L"<>:\"/|?*") != std::wstring::npos || std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32; })) {
        if (error) *error = TR("Windows doesn't allow this file name: it has a character such as ? * : or |.");
      } else if (tmp.size() >= MAX_PATH) {  // Windows' long paths are off (Excel takes 218 characters at most)
        if (error) *error = TR("The path is too long for Windows (260 characters at most).");
      } else if (target.has_parent_path() && !std::filesystem::is_directory(target.parent_path(), ec)) {
        if (error) *error = TR("The folder doesn't exist.");
      } else if (error) {
        *error = TR("Couldn't write the temporary file (is the folder read-only?)");
      }
      return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) {
      f.close();
      ::DeleteFileW(tmp.c_str());  // a half-written copy isn't left beside the file
      if (error) *error = TR("Write failed (out of disk space?)");
      return false;
    }
  }
  if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(tmp.c_str());
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (error) {
      *error = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)
                   ? TR("The file is read-only.")
                   : TR("Couldn't replace the original file (is it open in another program?)");
    }
    return false;
  }
  return true;
}

bool SaveYcg(const model::Workbook& wb, const std::wstring& path, const SaveOptions& opt, std::string* error) {
  std::string bytes = SaveYcgToBytes(wb, opt);
  if (!wb.open_password.empty()) {
    std::string enc;
    if (!EncryptYcg(bytes, wb.open_password, &enc)) {
      if (error) *error = TR("Couldn't encrypt the workbook.");
      return false;
    }
    bytes = std::move(enc);
  }
  return WriteFileAtomic(path, bytes, error);
}

LoadResult LoadYcg(const std::wstring& path, const std::string& password) {
  std::string bytes;
  if (!ReadFileBytes(path, &bytes)) {
    LoadResult r;
    r.error = TR("Couldn't read the file.");
    return r;
  }
  if (bytes.empty()) {
    // empty file (e.g. created by New -> YCGrid Workbook): treated as a new workbook
    LoadResult r;
    r.ok = true;
    r.wb = model::Workbook::NewDefault(TR("Sheet"));
    return r;
  }
  return LoadYcgFromBytes(bytes, password);
}

void WriteCellValue(MsgWriter& w, const core::Value& v) { WriteMsgValue(w, v); }
bool ReadCellValue(MsgReader& r, core::Value* v) { return ReadMsgValue(r, v); }
void WriteCellStyle(MsgWriter& w, const model::Style& s, const model::StyleTable& t) { WriteStyle(w, s, t); }
bool ReadCellStyle(MsgReader& r, model::Style* s, model::StyleTable& t) { return ReadStyle(r, s, t); }

}  // namespace yc::io
