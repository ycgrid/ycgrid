// SPDX-License-Identifier: Apache-2.0
// notebook_ui.cpp - the code cells of workbooks saved with the former Notebook window (NBOK chunk). The notebook was
// removed (ADR 0011); the cells stay in the workbook as they are (hard rule 21), and yc.notebook reads them or writes
// them to a plain .ipynb.
#include "app_state.h"

#include "engine/engine.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace yc::app {

using io::MV;

namespace {

const std::vector<std::string>& Cells(const AppState& s) {
  static const std::vector<std::string> kNone;
  return s.snap ? *s.snap->wb.notebook : kNone;
}

bool ExportIpynb(const std::vector<std::string>& codes, const std::wstring& path, std::string* err) {
  nlohmann::json cells = nlohmann::json::array();
  for (const auto& code : codes) {
    nlohmann::json src = nlohmann::json::array();
    size_t a = 0;
    while (a < code.size()) {  // nbformat keeps each line with its newline
      const size_t b = code.find('\n', a);
      src.push_back(code.substr(a, b == std::string::npos ? std::string::npos : b + 1 - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    cells.push_back({{"cell_type", "code"}, {"metadata", nlohmann::json::object()}, {"source", src}, {"execution_count", nullptr},
                     {"outputs", nlohmann::json::array()}});
  }
  const nlohmann::json nb = {{"cells", cells},
                             {"metadata", {{"kernelspec", {{"display_name", "Python 3"}, {"language", "python"}, {"name", "python3"}}},
                                           {"language_info", {{"name", "python"}}}}},
                             {"nbformat", 4},
                             {"nbformat_minor", 5}};
  std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  if (!f) {
    *err = "Can't write " + ToUtf8(path);
    return false;
  }
  f << nb.dump(1);
  return static_cast<bool>(f);
}

}  // namespace

// yc.notebook: the kept cells (read only) and their export
io::MV NotebookRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err) {
  if (m == "notebook.cells") {
    MV a = MV::Array();
    for (const auto& code : Cells(s)) {
      MV x = MV::Map();
      x.Set("code", MV::Str(code));
      a.Push(std::move(x));
    }
    return a;
  }
  if (m == "notebook.export") {
    const std::wstring path = FromUtf8(p.GetStr("path"));
    if (path.empty()) {
      *err = "A path to a .ipynb file is needed";
      return MV();
    }
    if (!ExportIpynb(Cells(s), path, err)) return MV();
    return MV::Int(static_cast<int64_t>(Cells(s).size()));
  }
  *err = "The notebook was removed (ADR 0011): yc.notebook.cells() and export_ipynb() read the cells old workbooks kept";
  return MV();
}

}  // namespace yc::app
