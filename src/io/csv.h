// SPDX-License-Identifier: Apache-2.0
// csv.h - CSV / TSV import and export (PLAN §12)
//
// Import: detects the encoding (UTF-8 BOM, UTF-16 LE/BE; without a BOM try UTF-8 first, then the system ANSI code page)
// and the separator (comma, tab, semicolon); quoted fields may contain line breaks; every field is parsed with the user input rules,
// numbers with more than 15 significant digits are stored as Decimal (PLAN §5.2a).
// Export: displayed text (number formats applied), Decimals with all digits; UTF-8 with BOM (Excel opens it without garbling).
#pragma once

#include "engine/snapshot.h"
#include "model/workbook.h"

#include <string>
#include <string_view>
#include <vector>

namespace yc::io {

struct CsvTable {
  std::vector<std::vector<std::string>> rows;
  char delimiter = ',';
};

std::string DecodeText(std::string_view bytes);  // any encoding -> UTF-8
CsvTable ParseCsv(std::string_view utf8, char delimiter = 0);  // 0 = detect
// CSV -> new workbook (one sheet, named after the file)
bool ImportCsv(const std::wstring& path, model::Workbook* wb, std::string* error);
// sheet -> CSV
bool ExportCsv(const engine::Snapshot& snap, uint32_t sheet, const std::wstring& path, std::string* error, char delimiter = ',');

}  // namespace yc::io
