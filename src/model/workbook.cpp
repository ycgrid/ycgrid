// SPDX-License-Identifier: Apache-2.0
#include "model/workbook.h"

#include <cctype>

namespace yc::model {

namespace {
bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
    if (x < 0x80 && y < 0x80) {
      if (std::tolower(x) != std::tolower(y)) return false;
    } else if (x != y) {
      return false;
    }
  }
  return true;
}
}  // namespace

int32_t Workbook::IndexOfId(uint32_t id) const {
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (sheets[i]->id == id) return static_cast<int32_t>(i);
  }
  return -1;
}

int32_t Workbook::FindSheet(std::string_view name) const {
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (IEquals(sheets[i]->name, name)) return static_cast<int32_t>(i);
  }
  return -1;
}

const Sheet* Workbook::SheetById(uint32_t id) const { return SheetAt(IndexOfId(id)); }
Sheet* Workbook::MutSheetById(uint32_t id) { return MutSheetAt(IndexOfId(id)); }

std::string Workbook::NextSheetName(const char* prefix) const {
  for (int n = static_cast<int>(sheets.size()) + 1;; ++n) {
    std::string s = std::string(prefix) + std::to_string(n);
    if (FindSheet(s) < 0) return s;
  }
}

Workbook Workbook::NewDefault(const char* sheet_prefix) {
  Workbook wb;
  Sheet s;
  s.id = wb.next_sheet_id++;
  s.name = std::string(sheet_prefix) + "1";
  s.code_name = "Sheet1";
  wb.sheets.emplace_back(std::make_shared<Sheet>(std::move(s)));
  return wb;
}

void Workbook::FinalizeForSnapshot() const {
  for (const auto& s : sheets) s->Finalize();
}

}  // namespace yc::model
