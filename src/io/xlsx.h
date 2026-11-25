// SPDX-License-Identifier: Apache-2.0
// xlsx.h - Excel workbooks (.xlsx / .xlsm): open and save a copy
//
// Import: sheets, values, formulas (shared and array formulas; _xlfn. prefixes dropped), number formats, fonts, fills,
// borders, alignment, merged cells, column widths, row heights, hidden sheets / rows / columns and defined names.
// Export: the same, with Excel's _xlfn. prefixes added for newer functions and the cached values written so Excel shows
// them before recalculating. Not carried over: charts, pictures, controls, PivotTables, conditional formatting, data
// validation, notes and the Python project (they stay in the .ycg). What a file holds of these is counted
// (XlsxSkipped) so the person is told what the opened workbook or the saved copy lacks (ADR 0004).
#pragma once

#include "engine/snapshot.h"
#include "model/workbook.h"

#include <string>
#include <string_view>
#include <vector>

namespace yc::io {

// What an Excel workbook holds that YCGrid doesn't carry over: counted over the whole workbook when one is opened
// (from its parts) or before a copy is written (from the model), and told to the person. Lines() gives one line a
// kind in the UI's language ("Charts: 3"), only for the kinds present.
struct XlsxSkipped {
  int charts = 0, pictures = 0, shapes = 0, controls = 0, pivot_tables = 0, slicers = 0, tables = 0;
  int cond_formats = 0, validations = 0, comments = 0, hyperlinks = 0, sparklines = 0;
  int frozen_sheets = 0, protected_sheets = 0, external_links = 0, queries = 0, panels = 0;
  bool macros = false;  // Excel macros (xl/vbaProject.bin): YCGrid runs Python instead
  bool Any() const;
  std::vector<std::string> Lines() const;
};

bool IsXlsxPath(const std::wstring& path);
bool ImportXlsx(const std::wstring& path, model::Workbook* wb, std::string* error, std::vector<std::string>* warnings,
                XlsxSkipped* skipped = nullptr);
bool ImportXlsxBytes(std::string_view bytes, model::Workbook* wb, std::string* error, std::vector<std::string>* warnings,
                     XlsxSkipped* skipped = nullptr);
bool ExportXlsx(const model::Workbook& wb, const std::wstring& path, std::string* error);
std::string ExportXlsxBytes(const model::Workbook& wb);
XlsxSkipped XlsxExportSkipped(const model::Workbook& wb);  // what ExportXlsx leaves out of this workbook

// the ZIP container (stored / deflated entries; used by xlsx and its tests)
struct ZipEntry {
  std::string name, data;
};
bool ReadZip(std::string_view bytes, std::vector<ZipEntry>* out, std::string* error);
std::string WriteZip(const std::vector<ZipEntry>& entries);

}  // namespace yc::io
