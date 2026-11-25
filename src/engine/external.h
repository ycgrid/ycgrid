// SPDX-License-Identifier: Apache-2.0
// external.h - references to other workbooks: =[Book2.ycg]Sheet1!A1, ='C:\dir\[Book2.xlsx]Sheet 1'!A1:B3
//
// A referenced sheet of another workbook gets a sheet id of its own (kExtSheetBase and up), so parsing, text restoration and
// evaluation use the usual sheet-id paths; its values come from the workbook's cache (Workbook::external_links), which the
// app fills from the source file (Data > Edit Links > Update Values) and which is saved with the workbook.
#pragma once

#include "core/geometry.h"
#include "core/value.h"
#include "model/workbook.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace yc::engine {

inline constexpr int32_t kExtSheetBase = 0xC000;  // below 0x10000: sheet ids are kept in 16 bits of the cell keys
inline constexpr int32_t kExtSheetsPerLink = 64;
inline constexpr int32_t kExtMaxLinks = (0x10000 - kExtSheetBase) / kExtSheetsPerLink;  // 256

inline bool IsExternalSheet(int32_t id) { return id >= kExtSheetBase && id < 0x10000; }
inline int32_t ExternalSheetId(size_t link, size_t sheet) {
  return kExtSheetBase + static_cast<int32_t>(link) * kExtSheetsPerLink + static_cast<int32_t>(sheet);
}
inline size_t ExternalLinkOf(int32_t id) { return static_cast<size_t>((id - kExtSheetBase) / kExtSheetsPerLink); }
inline size_t ExternalSheetOf(int32_t id) { return static_cast<size_t>((id - kExtSheetBase) % kExtSheetsPerLink); }

// "C:\dir\[Book2.ycg]Sheet1" -> path "C:\dir\Book2.ycg", sheet "Sheet1"; false when it isn't an external sheet name
bool SplitExternalName(std::string_view name, std::string* path, std::string* sheet);
// the id of an external sheet name; register = add the link / sheet when missing (else -1)
int32_t FindExternalSheet(model::Workbook& wb, std::string_view name, bool register_it);
int32_t FindExternalSheet(const model::Workbook& wb, std::string_view name);
// how the sheet is written in formulas: [Book2.ycg]Sheet1 (the source in this workbook's folder) or C:\dir\[Book2.ycg]Sheet1
std::string ExternalSheetName(const model::Workbook& wb, int32_t id);
// a cached value (empty when unknown) and the extent of the cached values of a sheet
const core::Value& ExternalValue(const model::Workbook& wb, int32_t id, uint32_t r, uint32_t c);
core::Rect ExternalUsed(const model::Workbook& wb, int32_t id, bool* any);

}  // namespace yc::engine
