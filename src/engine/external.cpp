// SPDX-License-Identifier: Apache-2.0
// external.cpp - links to other workbooks
#include "engine/external.h"

#include <algorithm>
#include <cctype>

namespace yc::engine {

namespace {

std::string LowerAscii(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

std::string FileNameOf(std::string_view path) {
  const size_t k = path.find_last_of("\\/");
  return std::string(k == std::string_view::npos ? path : path.substr(k + 1));
}

std::string DirOf(std::string_view path) {
  const size_t k = path.find_last_of("\\/");
  return k == std::string_view::npos ? std::string() : std::string(path.substr(0, k));
}

// the same source: the same full path, or the same file name when one side has no folder
bool SameSource(const std::string& a, const std::string& b) {
  if (LowerAscii(a) == LowerAscii(b)) return true;
  if (DirOf(a).empty() || DirOf(b).empty()) return LowerAscii(FileNameOf(a)) == LowerAscii(FileNameOf(b));
  return false;
}

}  // namespace

bool SplitExternalName(std::string_view name, std::string* path, std::string* sheet) {
  const size_t open = name.find('['), close = name.find(']');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open + 2) return false;
  *path = std::string(name.substr(0, open)) + std::string(name.substr(open + 1, close - open - 1));
  *sheet = std::string(name.substr(close + 1));
  return !sheet->empty();
}

int32_t FindExternalSheet(model::Workbook& wb, std::string_view name, bool register_it) {
  std::string path, sheet;
  if (!SplitExternalName(name, &path, &sheet)) return -1;
  const auto& links = *wb.external_links;
  for (size_t i = 0; i < links.size(); ++i) {
    if (!SameSource(links[i].path, path)) continue;
    for (size_t j = 0; j < links[i].sheets.size(); ++j) {
      if (LowerAscii(links[i].sheets[j]) == LowerAscii(sheet)) return ExternalSheetId(i, j);
    }
    if (!register_it || static_cast<int32_t>(links[i].sheets.size()) >= kExtSheetsPerLink) return -1;
    auto& l = wb.external_links.mut()[i];
    l.sheets.push_back(sheet);
    l.values.emplace_back();
    return ExternalSheetId(i, l.sheets.size() - 1);
  }
  if (!register_it || static_cast<int32_t>(links.size()) >= kExtMaxLinks) return -1;
  model::ExternalLink l;
  // a file name alone means this workbook's folder (like Excel)
  l.path = DirOf(path).empty() && !wb.path.empty() ? DirOf(wb.path) + "\\" + path : path;
  l.sheets.push_back(sheet);
  l.values.emplace_back();
  wb.external_links.mut().push_back(std::move(l));
  return ExternalSheetId(wb.external_links->size() - 1, 0);
}

int32_t FindExternalSheet(const model::Workbook& wb, std::string_view name) {
  return FindExternalSheet(const_cast<model::Workbook&>(wb), name, false);
}

std::string ExternalSheetName(const model::Workbook& wb, int32_t id) {
  const size_t li = ExternalLinkOf(id), si = ExternalSheetOf(id);
  const auto& links = *wb.external_links;
  if (li >= links.size() || si >= links[li].sheets.size()) return {};
  const auto& l = links[li];
  const std::string dir = DirOf(l.path);
  const bool here = dir.empty() || (!wb.path.empty() && LowerAscii(dir) == LowerAscii(DirOf(wb.path)));
  return (here ? std::string() : dir + "\\") + "[" + FileNameOf(l.path) + "]" + l.sheets[si];
}

const core::Value& ExternalValue(const model::Workbook& wb, int32_t id, uint32_t r, uint32_t c) {
  static const core::Value kEmpty;
  const size_t li = ExternalLinkOf(id), si = ExternalSheetOf(id);
  const auto& links = *wb.external_links;
  if (li >= links.size() || si >= links[li].values.size()) return kEmpty;
  const auto& m = links[li].values[si];
  auto it = m.find((uint64_t{r} << 16) | c);
  return it == m.end() ? kEmpty : it->second;
}

core::Rect ExternalUsed(const model::Workbook& wb, int32_t id, bool* any) {
  *any = false;
  const size_t li = ExternalLinkOf(id), si = ExternalSheetOf(id);
  const auto& links = *wb.external_links;
  if (li >= links.size() || si >= links[li].values.size() || links[li].values[si].empty()) return core::Rect{0, 0, 0, 0};
  core::Rect u{UINT32_MAX, UINT32_MAX, 0, 0};
  for (const auto& [k, v] : links[li].values[si]) {
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    u.r0 = std::min(u.r0, r);
    u.c0 = std::min(u.c0, c);
    u.r1 = std::max(u.r1, r);
    u.c1 = std::max(u.c1, c);
  }
  *any = true;
  return core::Rect{0, 0, u.r1, u.c1};
}

}  // namespace yc::engine
