// SPDX-License-Identifier: Apache-2.0
#include "model/style.h"

#include <cstring>
#include <functional>

namespace yc::model {

bool Style::operator==(const Style& o) const {
  if (font_name != o.font_name || font_size != o.font_size || font_color != o.font_color || bold != o.bold ||
      italic != o.italic || strike != o.strike || underline != o.underline || fill_color != o.fill_color ||
      halign != o.halign || valign != o.valign || indent != o.indent || wrap != o.wrap || shrink != o.shrink ||
      rotation != o.rotation || numfmt != o.numfmt || locked != o.locked || hidden != o.hidden || pattern != o.pattern ||
      pattern_color != o.pattern_color || gradient != o.gradient || gradient_angle != o.gradient_angle || gradient_color2 != o.gradient_color2 || script != o.script || cell_style != o.cell_style ||
      justify_last_line != o.justify_last_line || reading_order != o.reading_order) {
    return false;
  }
  for (int i = 0; i < kEdgeCount; ++i) {
    if (!(border[i] == o.border[i])) return false;
  }
  return true;
}

size_t Style::Hash() const {
  size_t h = 1469598103934665603ULL;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  uint32_t fs;
  std::memcpy(&fs, &font_size, 4);
  mix(font_name);
  mix(fs);
  mix(font_color);
  mix((bold ? 1u : 0u) | (italic ? 2u : 0u) | (strike ? 4u : 0u) | (static_cast<uint32_t>(underline) << 3) | (static_cast<uint32_t>(script) << 8));
  mix(fill_color);
  for (int i = 0; i < kEdgeCount; ++i) mix((uint64_t{border[i].color} << 8) | static_cast<uint64_t>(border[i].style));
  mix(static_cast<uint64_t>(halign) | (static_cast<uint64_t>(valign) << 8) | (uint64_t{indent} << 16) |
      (wrap ? 1ull << 24 : 0) | (shrink ? 1ull << 25 : 0) | (locked ? 1ull << 26 : 0) | (hidden ? 1ull << 27 : 0));
  mix(static_cast<uint64_t>(static_cast<uint16_t>(rotation)));
  mix(numfmt);
  mix(static_cast<uint64_t>(pattern) | (static_cast<uint64_t>(gradient) << 8) | (static_cast<uint64_t>(static_cast<uint16_t>(gradient_angle)) << 16));
  mix(pattern_color);
  mix(gradient_color2);
  mix(cell_style);
  mix((justify_last_line ? 1u : 0u) | (static_cast<uint32_t>(reading_order) << 1));
  return h;
}

uint32_t StyleTable::FindString(const std::string& s) const {
  auto it = string_index_.find(s);
  return it == string_index_.end() ? 0 : it->second;
}

StyleTable::StyleTable() {
  styles_.push_back(Style{});
  styles_.push_back(Style{});  // kStyleOwnNormal, never found by Intern
  index_.emplace(Style{}, 0);
  strings_.push_back("");
  string_index_.emplace("", 0);
}

uint32_t StyleTable::Intern(const Style& s) {
  auto it = index_.find(s);
  if (it != index_.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(styles_.size());
  styles_.push_back(s);
  index_.emplace(s, id);
  return id;
}

uint32_t StyleTable::InternString(const std::string& s) {
  auto it = string_index_.find(s);
  if (it != string_index_.end()) return it->second;
  const uint32_t id = static_cast<uint32_t>(strings_.size());
  strings_.push_back(s);
  string_index_.emplace(s, id);
  return id;
}

const std::string& StyleTable::FontName(const Style& s) const { return s.font_name ? Str(s.font_name) : default_font; }

const std::string& StyleTable::NumberFormat(const Style& s) const { return s.numfmt ? Str(s.numfmt) : general_; }

}  // namespace yc::model
