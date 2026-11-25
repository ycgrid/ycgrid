// SPDX-License-Identifier: Apache-2.0
// fn_cube.cpp - values the CUBE functions left in older workbooks (text with a marker) show their caption. The
// functions went with the Data Model (ADR 0012): CUBEVALUE and the others are now unknown names.
#include "formula/functions.h"

namespace yc::formula {

bool ParseCubeValue(std::string_view t, std::string* caption) {
  constexpr std::string_view kTag = "\x1b" "CUBE\x1f";
  if (t.size() < kTag.size() + 3 || t.substr(0, kTag.size()) != kTag || t[kTag.size() + 1] != '\x1f') return false;
  const size_t c0 = kTag.size() + 2, c1 = t.find('\x1f', c0);
  if (c1 == std::string_view::npos) return false;
  if (caption) *caption = std::string(t.substr(c0, c1 - c0));
  return true;
}

}  // namespace yc::formula
