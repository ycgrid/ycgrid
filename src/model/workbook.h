// SPDX-License-Identifier: Apache-2.0
// workbook.h - workbook (PLAN §5)
//
// Every member is COW: publishing a snapshot = copying the Workbook (pointers only); the undo stack also stores old Workbooks directly,
// costing only the tiles that get rewritten later (ADR 0001).
#pragma once

#include "core/cow.h"
#include "core/value.h"
#include "model/bind.h"
#include "model/safety.h"
#include "model/formula_table.h"
#include "model/objects.h"
#include "model/sheet.h"
#include "model/style.h"
#include "model/theme.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace yc::model {

// custom document properties (Excel CustomDocumentProperties)
struct DocProp {
  core::Value value;
  bool link_to_content = false;
  std::string link_source;
  bool operator==(const DocProp&) const = default;
};

// containers (PLAN §10.5): definitions live in the workbook; positions and the docking tree in LAYOUT
struct ContainerDef {
  std::string name, caption;
  double left = 100, top = 100, width = 640, height = 420;
  bool floating = true;
  bool created_by_python = false;
  bool operator==(const ContainerDef&) const = default;
};


// a workbook that formulas refer to (=[Book2.ycg]Sheet1!A1): the source and the values last read from it
struct ExternalLink {
  std::string path;                 // absolute when known; a file name alone = this workbook's folder
  std::vector<std::string> sheets;  // the referenced sheets (a reference uses engine::ExternalSheetId(link, sheet))
  std::vector<std::map<uint64_t, core::Value>> values;  // per sheet: row << 16 | column -> value
  double updated = 0;               // when the values were read (epoch seconds; 0 = never)
  std::string error;                // the last update's problem ("" = fine)
  bool operator==(const ExternalLink&) const = default;
};

// chunks we can't read but that are marked "preserve as is" (a file saved by a newer version opened in an older one, PLAN §15.3)
struct RawChunk {
  uint32_t fourcc = 0;
  uint16_t version = 0;
  uint16_t flags = 0;
  std::string data;  // uncompressed contents
  bool operator==(const RawChunk&) const = default;
};

class Workbook {
 public:
  std::vector<core::Cow<Sheet>> sheets;  // in tab order
  uint32_t next_sheet_id = 1;
  uint32_t next_object_id = 1;
  core::Cow<FormulaTable> formulas;
  core::Cow<StyleTable> styles;
  core::Cow<std::vector<NameDef>> names;
  core::Cow<std::map<std::string, std::string>> store;      // ThisWorkbook.Store (values are msgpack bytes)
  core::Cow<std::map<std::string, DocProp>> doc_props;      // CustomDocumentProperties
  core::Cow<std::vector<Panel>> panels;
  core::Cow<std::vector<ContainerDef>> containers;
  core::Cow<Theme> theme;                  // Page Layout -> Themes (THEM chunk)
  core::Cow<std::vector<NamedStyle>> cell_styles;  // custom cell styles (CSTY chunk)
  bool legacy_decimal = false;  // loading a file from before DECIMAL(text, radix): DECIMAL(x, places) reads as DECIMAL.SCALE
  core::Cow<std::vector<ExternalLink>> external_links;  // other workbooks formulas refer to (EXTL chunk)
  core::Cow<std::vector<BoundTable>> bound_tables;  // [YC] tables fed by Python by key (BIND chunk)
  // [YC] safety for live trading (SAFE chunk): parameter cells, the change log, settings
  core::Cow<std::vector<ParameterDef>> parameters;
  core::Cow<std::vector<AuditEntry>> audit_log;
  core::Cow<SafetySettings> safety;
  core::Cow<std::vector<AlertRule>> alerts;  // [YC] alert rules (SAFE chunk, key 4)
  core::Cow<std::vector<std::string>> notebook;  // [YC] the former Notebook window's code cells (NBOK chunk, ADR 0011)
  // co-authoring: how many records of each co-author's log the saved workbook contains (COAU chunk); someone who
  // opens it while others edit replays the rest
  std::string layout_json;  // LAYOUT chunk: produced and interpreted by the UI; the engine only stores it
  // Review -> Protect Workbook: the structure (add, delete, rename, move, hide sheets) and windows
  bool protect_structure = false, protect_windows = false;
  uint16_t protect_hash = 0;
  // iterative calculation as the workbook was saved with it (Excel keeps it in the file, and opening the file sets
  // it: WKBK key 7); -1 = not saved (older files): the session's settings stay. The engine's publish fills it in.
  int8_t calc_iterate = -1;
  int32_t calc_max_iterations = 100;
  double calc_max_change = 0.001;
  // ... and the calculation mode with them: 0 automatic, 1 manual, 2 automatic except for data tables; -1 = not saved
  int8_t calc_mode = -1;
  // File > Options > Advanced > When calculating this workbook (saved with it, Excel 16 measured; WKBK key 8, the
  // .xlsx's workbookPr date1904 and calcPr fullPrecision / forceFullCalc): the 1904 date system, values rounded to
  // what their format shows (Set precision as displayed), every calculation a full one
  bool date1904 = false, precision_as_displayed = false, force_full_calc = false;
  int32_t active_sheet = 0;  // index
  std::string path;          // current file (empty = not saved)
  bool dirty = false;        // has unsaved changes
  bool read_only = false;    // the file contains required content this version can't read (PLAN §15.3)
  std::string read_only_reason;
  std::string open_password;  // File -> Info -> Encrypt with Password: the file is saved encrypted (kept in memory only)
  uint16_t file_format_major = 0, file_format_minor = 0;  // 0 = new workbook
  core::Cow<std::vector<RawChunk>> unknown_chunks;  // COW: copying the snapshot doesn't copy the contents

  // queries
  int32_t IndexOfId(uint32_t id) const;
  int32_t FindSheet(std::string_view name) const;  // case-insensitive; returns the index
  const Sheet* SheetById(uint32_t id) const;
  Sheet* MutSheetById(uint32_t id);  // COW
  const Sheet* SheetAt(int32_t index) const {
    return index >= 0 && index < static_cast<int32_t>(sheets.size()) ? &sheets[static_cast<size_t>(index)].get() : nullptr;
  }
  Sheet* MutSheetAt(int32_t index) {
    return index >= 0 && index < static_cast<int32_t>(sheets.size()) ? &sheets[static_cast<size_t>(index)].mut() : nullptr;
  }

  // create the default blank workbook (one sheet, "Sheet1")
  static Workbook NewDefault(const char* sheet_prefix = "Sheet");
  std::string NextSheetName(const char* prefix = "Sheet") const;

  void FinalizeForSnapshot() const;
};

}  // namespace yc::model
