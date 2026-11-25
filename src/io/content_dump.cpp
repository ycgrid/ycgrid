// SPDX-License-Identifier: Apache-2.0
#include "io/content_dump.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "core/geometry.h"
#include "engine/snapshot.h"
#include "io/ycg.h"

namespace yc::io {

namespace {

uint64_t LE(std::string_view b, size_t at, int n) {
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<size_t>(i)]);
  return v;
}

std::string ValueText(const core::Value& v) {
  char buf[64];
  if (v.IsEmpty()) return "-";
  if (v.IsNumeric()) {
    std::snprintf(buf, sizeof(buf), "%.17g", v.AsDouble());
    return buf;
  }
  if (v.IsBool()) return v.boolean() ? "TRUE" : "FALSE";
  if (v.IsError()) return core::ErrorText(v.error());
  if (v.IsString()) return "\"" + std::string(v.str()) + "\"";
  return "?";
}

// what a person sees of a format, then the style's hash (two styles that look alike but differ show it)
std::string StyleText(const model::StyleTable& t, uint32_t id) {
  if (id == 0) return "";
  const model::Style& s = t.Get(id);
  std::string out;
  if (s.bold) out += "b ";
  if (s.italic) out += "i ";
  if (s.underline != decltype(s.underline){}) out += "u ";
  if (s.strike) out += "s ";
  if (s.fill_color) out += "fill ";
  if (s.font_color) out += "color ";
  for (const auto& e : s.border) {
    if (e.style != decltype(e.style){}) {
      out += "border ";
      break;
    }
  }
  if (s.wrap) out += "wrap ";
  if (!s.locked) out += "unlocked ";
  if (s.numfmt) out += "[" + t.NumberFormat(s) + "] ";
  // the hash of what the style says, not of the string table's numbers (they differ between a workbook and the same
  // workbook saved and opened again)
  model::Style plain = s;
  plain.font_name = plain.numfmt = plain.cell_style = 0;
  size_t h = plain.Hash();
  for (const uint32_t sid : {s.font_name, s.numfmt, s.cell_style}) {
    h = h * 1099511628211ull ^ std::hash<std::string_view>{}(sid ? t.Str(sid) : std::string_view());
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "#%016llx", static_cast<unsigned long long>(h));
  return out + buf;
}

std::string DimText(const model::DimIndex& d, char kind) {
  std::string out;
  for (const auto& e : d.Entries()) {
    if (e.size == d.default_size() && e.saved == 0) continue;
    char buf[96];
    const std::string at = kind == 'C' ? core::ColumnName(static_cast<uint32_t>(e.index)) : std::to_string(e.index + 1);
    std::snprintf(buf, sizeof(buf), "  %s %s size %d%s\n", kind == 'C' ? "col" : "row", at.c_str(), e.size, e.size == 0 ? " hidden" : "");
    out += buf;
  }
  return out;
}

}  // namespace

std::string ContentDump(const model::Workbook& wb_in) {
  std::string out;
  const model::StyleTable& st = *wb_in.styles;
  for (const auto& sp : wb_in.sheets) {
    const model::Sheet& s = *sp;
    out += "sheet " + s.name + (s.visible ? "" : " (hidden)") + "\n";
    // the cells holding something, row by row
    std::set<std::pair<uint32_t, uint32_t>> cells;
    s.values.ForEach([&](uint32_t rt, uint32_t ct, const model::ValueTile& t) {
      for (uint32_t i = 0; i < model::kTileCells; ++i) {
        if (!t.v[i].IsEmpty()) cells.insert({rt * model::kTileRows + i % model::kTileRows, ct * model::kTileCols + i / model::kTileRows});
      }
    });
    s.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0 && t.style_count == 0) return;
      for (uint32_t i = 0; i < model::kTileCells; ++i) {
        if (t.formula[i] || t.style[i]) cells.insert({rt * model::kTileRows + i % model::kTileRows, ct * model::kTileCols + i / model::kTileRows});
      }
    });
    for (const auto& [r, c] : cells) {
      const std::string input = engine::InputTextOf(wb_in, s.id, r, c);
      out += "  " + core::CellA1(r, c) + "\t" + input + "\t" + ValueText(s.GetValue(r, c)) + "\t" + StyleText(st, s.GetOwnStyle(r, c)) + "\n";
    }
    out += DimText(*s.rows, 'R');
    out += DimText(*s.cols, 'C');
    std::vector<core::Rect> merges(s.merges->begin(), s.merges->end());
    std::sort(merges.begin(), merges.end(), [](const core::Rect& a, const core::Rect& b) { return std::tie(a.r0, a.c0) < std::tie(b.r0, b.c0); });
    for (const auto& m : merges) out += "  merged " + core::CellA1(m.r0, m.c0) + ":" + core::CellA1(m.r1, m.c1) + "\n";
    for (const auto& [r, id] : *s.row_styles) out += "  row " + std::to_string(r + 1) + " format " + StyleText(st, id) + "\n";
    for (const auto& [c, id] : *s.col_styles) out += "  col " + core::ColumnName(c) + " format " + StyleText(st, id) + "\n";
  }
  // everything else the file keeps, chunk by chunk, with the window's state left out
  model::Workbook wb = wb_in;
  for (auto& sp : wb.sheets) {
    model::Sheet& s = sp.mut();
    s.view = model::SheetView{};
    s.tab_selected = false;
  }
  wb.active_sheet = 0;
  wb.next_sheet_id = 0;
  wb.next_object_id = 0;
  SaveOptions opt;
  opt.compress = false;
  const std::string bytes = SaveYcgToBytes(wb, opt);
  if (bytes.size() >= 48) {
    const uint32_t count = static_cast<uint32_t>(LE(bytes, 28, 4));
    const uint64_t dir = LE(bytes, 32, 8);
    std::map<std::string, int> seen;  // the n-th chunk of a kind (one SHET per sheet)
    for (uint32_t i = 0; i < count && dir + uint64_t{i} * 40 + 40 <= bytes.size(); ++i) {
      const size_t at = static_cast<size_t>(dir) + size_t{i} * 40;
      const uint32_t fourcc = static_cast<uint32_t>(LE(bytes, at, 4));
      const std::string name{static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF), static_cast<char>((fourcc >> 16) & 0xFF),
                             static_cast<char>((fourcc >> 24) & 0xFF)};
      if (name == "META") continue;  // who saved it and when
      char buf[64];
      std::snprintf(buf, sizeof(buf), "chunk %s %d %08x\n", name.c_str(), seen[name]++,static_cast<unsigned>(LE(bytes, at + 36, 4)));
      out += buf;
    }
  }
  return out;
}

}  // namespace yc::io
