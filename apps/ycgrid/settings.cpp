// SPDX-License-Identifier: Apache-2.0
#include "settings.h"

#include "app_state.h"
#include "core/i18n.h"
#include "core/parallel.h"
#include "core/paths.h"

#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace yc::app {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

Settings g_settings;
json g_doc = json::object();  // the whole file as read: unknown keys survive a save

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  wchar_t* p = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
  if (p) ::CoTaskMemFree(p);
  return out;
}

template <class T>
void Read(const json& j, const char* section, const char* key, T* out) {
  const json* v = &j;
  if (section) {
    auto it = j.find(section);
    if (it == j.end() || !it->is_object()) return;
    v = &*it;
  }
  auto it = v->find(key);
  if (it == v->end()) return;
  try {
    *out = it->get<T>();
  } catch (const json::exception&) {
    // wrong type: keep the default
  }
}

void ReadPath(const json& j, const char* section, const char* key, std::wstring* out) {
  std::string s;
  Read(j, section, key, &s);
  if (!s.empty()) *out = FromUtf8(s);
}

// legacy %LOCALAPPDATA%\ycgrid\language (one line) from before settings.json
std::string LegacyLanguage() {
  std::ifstream f(fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"ycgrid" / L"language", std::ios::binary);
  std::string v;
  if (!f || !std::getline(f, v)) return {};
  while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
  return v;
}

bool SaveLanguageSetting(const char* code) {
  g_settings.language = code ? code : "";
  return SaveSettings();
}

}  // namespace

Settings& Config() { return g_settings; }

namespace {
// YCGRID_SETTINGS: another settings file (tests that restart a hidden instance read back what it saved)
std::wstring SettingsOverride() {
  wchar_t buf[MAX_PATH + 1];
  const DWORD n = ::GetEnvironmentVariableW(L"YCGRID_SETTINGS", buf, MAX_PATH + 1);
  return n > 0 && n <= MAX_PATH ? std::wstring(buf, n) : std::wstring();
}
}  // namespace

std::wstring SettingsPath() {
  if (std::wstring o = SettingsOverride(); !o.empty()) return o;
  return (fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"ycgrid" / L"settings.json").wstring();
}

void ApplyCalcThreads() {
  core::SetWorkerLimit(!g_settings.multi_threaded_calc ? 1 : static_cast<size_t>(std::max(0, g_settings.calc_thread_count)));
}

void LoadSettings() {
  g_settings = Settings{};
  g_doc = json::object();
  std::ifstream f(fs::path(SettingsPath()), std::ios::binary);
  if (f) {
    try {
      json j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
      if (j.is_object()) g_doc = std::move(j);
    } catch (const json::exception&) {
      // broken file: start from the defaults (it is rewritten on the next save)
    }
  }
  const json& j = g_doc;
  if (!j.contains("language")) g_settings.language = LegacyLanguage();
  Read(j, nullptr, "language", &g_settings.language);
  Read(j, "view", "gridlines", &g_settings.show_gridlines);
  Read(j, "view", "headings", &g_settings.show_headings);
  Read(j, "view", "formula_bar", &g_settings.show_formula_bar);
  Read(j, "view", "formula_bar_lines", &g_settings.formula_bar_lines);
  if (j.contains("page") && j["page"].is_object() && j["page"].contains("last_margins") && j["page"]["last_margins"].is_array() &&
      j["page"]["last_margins"].size() == 6) {
    for (size_t k = 0; k < 6; ++k) {
      const json& v = j["page"]["last_margins"][k];
      if (v.is_number()) g_settings.last_margins[k] = v.get<double>();
    }
  }
  g_settings.formula_bar_lines = std::clamp(g_settings.formula_bar_lines, 1, 20);
  Read(j, "formulas", "autocomplete", &g_settings.formula_autocomplete);
  Read(j, "formulas", "function_tooltips", &g_settings.function_tooltips);
  Read(j, "formulas", "error_checking", &g_settings.error_checking);
  Read(j, "formulas", "error_evaluate_to_error", &g_settings.err_evaluate_to_error);
  Read(j, "formulas", "error_number_as_text", &g_settings.err_number_as_text);
  Read(j, "formulas", "error_inconsistent_formula", &g_settings.err_inconsistent_formula);
  Read(j, "formulas", "error_text_date", &g_settings.err_text_date);
  Read(j, "formulas", "error_omitted_cells", &g_settings.err_omitted_cells);
  Read(j, "formulas", "error_unlocked_formula", &g_settings.err_unlocked_formula);
  Read(j, "formulas", "error_empty_cell_reference", &g_settings.err_empty_cell_reference);
  Read(j, "formulas", "error_list_data_validation", &g_settings.err_list_data_validation);
  Read(j, "formulas", "error_inconsistent_list_formula", &g_settings.err_inconsistent_list_formula);
  Read(j, "formulas", "error_misleading_format", &g_settings.err_misleading_format);
  Read(j, "formulas", "recent_functions", &g_settings.recent_functions);
  if (g_settings.recent_functions.size() > 10) g_settings.recent_functions.resize(10);
  Read(j, "insert", "recent_symbols", &g_settings.recent_symbols);
  if (g_settings.recent_symbols.size() > 20) g_settings.recent_symbols.resize(20);
  Read(j, "insert", "recent_shapes", &g_settings.recent_shapes);
  if (g_settings.recent_shapes.size() > 24) g_settings.recent_shapes.resize(24);
  Read(j, "proofing", "ignore_uppercase", &g_settings.spell_ignore_upper);
  Read(j, "proofing", "ignore_numbers", &g_settings.spell_ignore_numbers);
  Read(j, "proofing", "ignore_internet", &g_settings.spell_ignore_internet);
  Read(j, "proofing", "flag_repeated", &g_settings.spell_repeated);
  Read(j, "proofing", "autocorrect_removed", &g_settings.autocorrect_removed);
  Read(j, "proofing", "autocorrect_replace", &g_settings.autocorrect_replace);
  if (j.contains("proofing") && j["proofing"].is_object() && j["proofing"].contains("autocorrect") && j["proofing"]["autocorrect"].is_array()) {
    g_settings.autocorrect.clear();
    for (const auto& e : j["proofing"]["autocorrect"]) {
      if (e.is_array() && e.size() == 2 && e[0].is_string() && e[1].is_string()) g_settings.autocorrect.emplace_back(e[0].get<std::string>(), e[1].get<std::string>());
    }
  }
  Read(j, "status_bar", "average", &g_settings.status_average);
  Read(j, "status_bar", "count", &g_settings.status_count);
  Read(j, "status_bar", "numerical_count", &g_settings.status_numerical_count);
  Read(j, "status_bar", "minimum", &g_settings.status_min);
  Read(j, "status_bar", "maximum", &g_settings.status_max);
  Read(j, "status_bar", "sum", &g_settings.status_sum);
  Read(j, "status_bar", "cell_mode", &g_settings.status_cell_mode);
  Read(j, "general", "show_quick_analysis", &g_settings.show_quick_analysis);
  Read(j, "general", "standard_font", &g_settings.standard_font);
  Read(j, "general", "standard_font_size", &g_settings.standard_font_size);
  Read(j, "general", "sheets_in_new_workbook", &g_settings.sheets_in_new_workbook);
  Read(j, "editing", "show_paste_options", &g_settings.show_paste_options);
  Read(j, "editing", "show_insert_options", &g_settings.show_insert_options);
  Read(j, "editing", "alert_before_overwriting", &g_settings.alert_before_overwriting);
  Read(j, "editing", "large_operation_alert", &g_settings.large_operation_alert);
  Read(j, "editing", "large_operation_thousands", &g_settings.large_operation_thousands);
  g_settings.large_operation_thousands = std::clamp(g_settings.large_operation_thousands, 1, 1000000000);
  Read(j, "chart", "show_tip_names", &g_settings.show_chart_tip_names);
  Read(j, "chart", "show_tip_values", &g_settings.show_chart_tip_values);
  Read(j, "calculation", "before_save", &g_settings.calculate_before_save);
  Read(j, "calculation", "format_stale_values", &g_settings.format_stale_values);
  Read(j, "calculation", "multi_threaded", &g_settings.multi_threaded_calc);
  Read(j, "calculation", "threads", &g_settings.calc_thread_count);
  g_settings.calc_thread_count = std::clamp(g_settings.calc_thread_count, 0, 1024);
  Read(j, "formulas", "generate_getpivotdata", &g_settings.generate_getpivotdata);
  Read(j, "status_bar", "caps_lock", &g_settings.status_caps_lock);
  Read(j, "status_bar", "num_lock", &g_settings.status_num_lock);
  Read(j, "status_bar", "scroll_lock", &g_settings.status_scroll_lock);
  Read(j, "status_bar", "end_mode", &g_settings.status_end_mode);
  Read(j, "status_bar", "selection_mode", &g_settings.status_selection_mode);
  Read(j, "status_bar", "view_shortcuts", &g_settings.status_view_shortcuts);
  Read(j, "status_bar", "zoom_slider", &g_settings.status_zoom_slider);
  Read(j, "status_bar", "zoom", &g_settings.status_zoom);
  Read(j, "status_bar", "error_log", &g_settings.status_error_log);
  Read(j, "calculation", "automatic", &g_settings.auto_calc);
  Read(j, "calculation", "data_tables", &g_settings.auto_data_tables);
  Read(j, "calculation", "excel_15_digit_input", &g_settings.excel_15_digit_input);
  Read(j, "calculation", "iterative", &g_settings.iterative);
  Read(j, "calculation", "max_iterations", &g_settings.max_iterations);
  Read(j, "calculation", "max_change", &g_settings.max_change);
  g_settings.max_iterations = std::clamp(g_settings.max_iterations, 1, 32767);
  if (!(g_settings.max_change > 0)) g_settings.max_change = 0.001;
  Read(j, "formulas", "r1c1_reference_style", &g_settings.r1c1);
  Read(j, "editing", "cell_drag_and_drop", &g_settings.cell_drag_and_drop);
  Read(j, "data", "pivot_auto_group_dates", &g_settings.pivot_auto_group_dates);
  Read(j, "editing", "follow_links_on_click", &g_settings.follow_links_on_click);
  Read(j, "editing", "move_after_return", &g_settings.move_after_return);
  Read(j, "editing", "move_after_return_direction", &g_settings.move_after_return_dir);
  g_settings.move_after_return_dir = std::clamp(g_settings.move_after_return_dir, 0, 3);
  Read(j, "editing", "edit_directly_in_cell", &g_settings.edit_directly_in_cell);
  Read(j, "editing", "enable_autocomplete", &g_settings.enable_autocomplete);
  Read(j, "editing", "auto_flash_fill", &g_settings.auto_flash_fill);
  Read(j, "live_data", "publish_interval_ms", &g_settings.publish_interval_ms);
  g_settings.publish_interval_ms = std::clamp(g_settings.publish_interval_ms, 0, 5000);
  ReadPath(j, "projects", "folder", &g_settings.projects_folder);
  ReadPath(j, "editor", "vscode", &g_settings.vscode_path);
  Read(j, "editor", "restart_on_save", &g_settings.restart_on_save);
  if (j.contains("editing") && j["editing"].is_object() && j["editing"].contains("custom_lists") && j["editing"]["custom_lists"].is_array()) {
    for (const auto& l : j["editing"]["custom_lists"]) {
      if (!l.is_array()) continue;
      std::vector<std::string> items;
      for (const auto& x : l) {
        if (x.is_string()) items.push_back(x.get<std::string>());
      }
      if (!items.empty()) g_settings.custom_lists.push_back(std::move(items));
    }
  }
  Read(j, "save", "autorecover", &g_settings.autorecover);
  Read(j, "save", "autorecover_minutes", &g_settings.autorecover_minutes);
  g_settings.autorecover_minutes = std::clamp(g_settings.autorecover_minutes, 1, 120);
  ReadPath(j, "save", "autorecover_folder", &g_settings.autorecover_folder);
  if (j.contains("ribbon") && j["ribbon"].is_object()) {
    const json& rb = j["ribbon"];
    if (rb.contains("quick_access") && rb["quick_access"].is_array()) {
      g_settings.qat.clear();
      for (const auto& x : rb["quick_access"]) {
        if (x.is_string()) g_settings.qat.push_back(x.get<std::string>());
      }
      // the old default list gets the Screenshot button added since (a list the user changed stays as it is)
      if (g_settings.qat == std::vector<std::string>{"Save", "Undo", "Redo"}) g_settings.qat.push_back("Screenshot");
    }
  }
  Read(j, "ribbon", "display", &g_settings.ribbon_mode);
  Read(j, "errors", "log_to_file", &g_settings.errors_log_to_file);
  ReadPath(j, "errors", "log_folder", &g_settings.errors_log_folder);
  {  // Trust Center
    std::string ms;
    Read(j, "trust_center", "macro_settings", &ms);
    g_settings.macro_setting = ms == "disable_without_notification" ? 1 : ms == "enable_all" ? 2 : 0;
    Read(j, "trust_center", "message_bar", &g_settings.message_bar);
    Read(j, "trust_center", "allow_network_locations", &g_settings.allow_network_locations);
    Read(j, "trust_center", "disable_trusted_locations", &g_settings.disable_trusted_locations);
    Read(j, "trust_center", "allow_network_documents", &g_settings.allow_network_documents);
    Read(j, "trust_center", "disable_trusted_documents", &g_settings.disable_trusted_documents);
    Read(j, "trust_center", "protected_view_internet", &g_settings.pv_internet);
    Read(j, "trust_center", "protected_view_unsafe_locations", &g_settings.pv_unsafe_locations);
    Read(j, "trust_center", "protected_view_outlook", &g_settings.pv_outlook);
    if (j.contains("trust_center") && j["trust_center"].is_object() && j["trust_center"].contains("trusted_locations") &&
        j["trust_center"]["trusted_locations"].is_array()) {
      g_settings.trusted_locations_set = true;
      for (const auto& l : j["trust_center"]["trusted_locations"]) {
        if (!l.is_object()) continue;
        Settings::TrustedLocation t;
        Read(l, nullptr, "path", &t.path);
        Read(l, nullptr, "subfolders", &t.subfolders);
        Read(l, nullptr, "description", &t.description);
        Read(l, nullptr, "modified", &t.modified);
        if (!t.path.empty()) g_settings.trusted_locations.push_back(std::move(t));
      }
    }
  }
  g_settings.ribbon_mode = std::clamp(g_settings.ribbon_mode, 0, 2);
  i18n::SetLanguageSaver(&SaveLanguageSetting);
}

namespace {
bool g_save_enabled = true;
}  // namespace

void EnableSettingsSave(bool on) { g_save_enabled = on; }
bool UserStateSaved() { return g_save_enabled; }

namespace {
std::wstring g_last_desktop_open;
int g_desktop_opens = 0;
}  // namespace

std::wstring LastDesktopOpen() { return g_last_desktop_open; }
int DesktopOpens() { return g_desktop_opens; }

bool OpenOnDesktop(const std::wstring& target, const std::wstring& params, void* owner, int show) {
  g_last_desktop_open = target;
  ++g_desktop_opens;
  if (!g_save_enabled) return false;  // a hidden instance: nothing on the user's desktop
  // lesson-ok: shell-open the one place that opens things on the desktop
  const auto r = ::ShellExecuteW(static_cast<HWND>(owner), L"open", target.c_str(), params.empty() ? nullptr : params.c_str(), nullptr, show);
  return reinterpret_cast<INT_PTR>(r) > 32;
}

bool SaveSettings() {
  // a hidden instance (tests, agents): the user's settings file stays as it is (one named by YCGRID_SETTINGS is written)
  if (!g_save_enabled && SettingsOverride().empty()) return true;
  json& j = g_doc;
  if (!j.is_object()) j = json::object();
  auto section = [&](const char* name) -> json& {
    if (!j.contains(name) || !j[name].is_object()) j[name] = json::object();
    return j[name];
  };
  j["language"] = g_settings.language;
  section("view")["gridlines"] = g_settings.show_gridlines;
  section("view")["headings"] = g_settings.show_headings;
  section("view")["formula_bar"] = g_settings.show_formula_bar;
  section("view")["formula_bar_lines"] = g_settings.formula_bar_lines;
  if (g_settings.last_margins[0] >= 0) section("page")["last_margins"] = g_settings.last_margins;
  section("formulas")["autocomplete"] = g_settings.formula_autocomplete;
  section("formulas")["function_tooltips"] = g_settings.function_tooltips;
  section("formulas")["error_checking"] = g_settings.error_checking;
  section("formulas")["error_evaluate_to_error"] = g_settings.err_evaluate_to_error;
  section("formulas")["error_number_as_text"] = g_settings.err_number_as_text;
  section("formulas")["error_inconsistent_formula"] = g_settings.err_inconsistent_formula;
  section("formulas")["error_text_date"] = g_settings.err_text_date;
  section("formulas")["error_omitted_cells"] = g_settings.err_omitted_cells;
  section("formulas")["error_unlocked_formula"] = g_settings.err_unlocked_formula;
  section("formulas")["error_empty_cell_reference"] = g_settings.err_empty_cell_reference;
  section("formulas")["error_list_data_validation"] = g_settings.err_list_data_validation;
  section("formulas")["error_inconsistent_list_formula"] = g_settings.err_inconsistent_list_formula;
  section("formulas")["error_misleading_format"] = g_settings.err_misleading_format;
  section("formulas")["recent_functions"] = g_settings.recent_functions;
  section("insert")["recent_symbols"] = g_settings.recent_symbols;
  section("insert")["recent_shapes"] = g_settings.recent_shapes;
  section("proofing")["ignore_uppercase"] = g_settings.spell_ignore_upper;
  section("proofing")["ignore_numbers"] = g_settings.spell_ignore_numbers;
  section("proofing")["ignore_internet"] = g_settings.spell_ignore_internet;
  section("proofing")["flag_repeated"] = g_settings.spell_repeated;
  section("proofing")["autocorrect_removed"] = g_settings.autocorrect_removed;
  section("proofing")["autocorrect_replace"] = g_settings.autocorrect_replace;
  {
    json ac = json::array();
    for (const auto& [a, b] : g_settings.autocorrect) ac.push_back(json::array({a, b}));
    section("proofing")["autocorrect"] = ac;
  }
  section("status_bar")["average"] = g_settings.status_average;
  section("status_bar")["count"] = g_settings.status_count;
  section("status_bar")["numerical_count"] = g_settings.status_numerical_count;
  section("status_bar")["minimum"] = g_settings.status_min;
  section("status_bar")["maximum"] = g_settings.status_max;
  section("status_bar")["sum"] = g_settings.status_sum;
  section("status_bar")["cell_mode"] = g_settings.status_cell_mode;
  section("general")["show_quick_analysis"] = g_settings.show_quick_analysis;
  section("general")["standard_font"] = g_settings.standard_font;
  section("general")["standard_font_size"] = g_settings.standard_font_size;
  section("general")["sheets_in_new_workbook"] = g_settings.sheets_in_new_workbook;
  section("editing")["show_paste_options"] = g_settings.show_paste_options;
  section("editing")["show_insert_options"] = g_settings.show_insert_options;
  section("editing")["alert_before_overwriting"] = g_settings.alert_before_overwriting;
  section("editing")["large_operation_alert"] = g_settings.large_operation_alert;
  section("editing")["large_operation_thousands"] = g_settings.large_operation_thousands;
  section("chart")["show_tip_names"] = g_settings.show_chart_tip_names;
  section("chart")["show_tip_values"] = g_settings.show_chart_tip_values;
  section("calculation")["before_save"] = g_settings.calculate_before_save;
  section("calculation")["format_stale_values"] = g_settings.format_stale_values;
  section("calculation")["multi_threaded"] = g_settings.multi_threaded_calc;
  section("calculation")["threads"] = g_settings.calc_thread_count;
  section("formulas")["generate_getpivotdata"] = g_settings.generate_getpivotdata;
  section("status_bar")["caps_lock"] = g_settings.status_caps_lock;
  section("status_bar")["num_lock"] = g_settings.status_num_lock;
  section("status_bar")["scroll_lock"] = g_settings.status_scroll_lock;
  section("status_bar")["end_mode"] = g_settings.status_end_mode;
  section("status_bar")["selection_mode"] = g_settings.status_selection_mode;
  section("status_bar")["view_shortcuts"] = g_settings.status_view_shortcuts;
  section("status_bar")["zoom_slider"] = g_settings.status_zoom_slider;
  section("status_bar")["zoom"] = g_settings.status_zoom;
  section("status_bar")["error_log"] = g_settings.status_error_log;
  section("calculation")["automatic"] = g_settings.auto_calc;
  section("calculation")["data_tables"] = g_settings.auto_data_tables;
  section("calculation")["excel_15_digit_input"] = g_settings.excel_15_digit_input;
  section("calculation")["iterative"] = g_settings.iterative;
  section("calculation")["max_iterations"] = g_settings.max_iterations;
  section("calculation")["max_change"] = g_settings.max_change;
  section("formulas")["r1c1_reference_style"] = g_settings.r1c1;
  section("editing")["cell_drag_and_drop"] = g_settings.cell_drag_and_drop;
  section("data")["pivot_auto_group_dates"] = g_settings.pivot_auto_group_dates;
  section("editing")["follow_links_on_click"] = g_settings.follow_links_on_click;
  section("editing")["move_after_return"] = g_settings.move_after_return;
  section("editing")["move_after_return_direction"] = g_settings.move_after_return_dir;
  section("editing")["edit_directly_in_cell"] = g_settings.edit_directly_in_cell;
  section("editing")["enable_autocomplete"] = g_settings.enable_autocomplete;
  section("editing")["auto_flash_fill"] = g_settings.auto_flash_fill;
  section("live_data")["publish_interval_ms"] = g_settings.publish_interval_ms;
  section("projects")["folder"] = ToUtf8(g_settings.projects_folder);
  section("editor")["vscode"] = ToUtf8(g_settings.vscode_path);
  section("editor")["restart_on_save"] = g_settings.restart_on_save;
  section("editing")["custom_lists"] = g_settings.custom_lists;
  section("save")["autorecover"] = g_settings.autorecover;
  section("save")["autorecover_minutes"] = g_settings.autorecover_minutes;
  section("save")["autorecover_folder"] = ToUtf8(g_settings.autorecover_folder);
  section("ribbon")["quick_access"] = g_settings.qat;
  section("ribbon")["display"] = g_settings.ribbon_mode;
  section("errors")["log_to_file"] = g_settings.errors_log_to_file;
  section("errors")["log_folder"] = ToUtf8(g_settings.errors_log_folder);
  {
    json& tc = section("trust_center");
    tc["macro_settings"] = g_settings.macro_setting == 1 ? "disable_without_notification" : g_settings.macro_setting == 2 ? "enable_all" : "disable_with_notification";
    tc["message_bar"] = g_settings.message_bar;
    tc["allow_network_locations"] = g_settings.allow_network_locations;
    tc["disable_trusted_locations"] = g_settings.disable_trusted_locations;
    tc["allow_network_documents"] = g_settings.allow_network_documents;
    tc["disable_trusted_documents"] = g_settings.disable_trusted_documents;
    tc["protected_view_internet"] = g_settings.pv_internet;
    tc["protected_view_unsafe_locations"] = g_settings.pv_unsafe_locations;
    tc["protected_view_outlook"] = g_settings.pv_outlook;
    if (g_settings.trusted_locations_set) {
      json a = json::array();
      for (const auto& t : g_settings.trusted_locations) {
        a.push_back({{"path", t.path}, {"subfolders", t.subfolders}, {"description", t.description}, {"modified", t.modified}});
      }
      tc["trusted_locations"] = std::move(a);
    }
  }

  // write to a temporary file and swap it in, so a crash never leaves half a file
  const fs::path path = SettingsPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    if (!f) return false;
  }
  return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::wstring TempFolder() {
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);  // lesson-ok: temp-folder the one place that asks it
  if (ec || dir.empty()) {
    wchar_t buf[MAX_PATH + 1];
    const DWORD n = ::GetTempPathW(MAX_PATH + 1, buf);
    dir = n ? fs::path(std::wstring(buf, n)) : fs::path();
  }
  if (!dir.empty()) {
    fs::create_directories(dir, ec);
    if (fs::is_directory(dir, ec)) return dir.wstring();
  }
  const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
  dir = fs::path(local.empty() ? L"." : local) / L"ycgrid" / L"temp";
  fs::create_directories(dir, ec);
  return dir.wstring();
}

std::wstring ProjectsDir() {
  // a hidden instance (tests, agents) makes its projects (Book1, Book2...) in the temp folder, not in Documents
  if (!g_save_enabled) return (fs::path(TempFolder()) / L"YCGrid-hidden-projects").wstring();
  if (!g_settings.projects_folder.empty()) return g_settings.projects_folder;
  std::wstring docs = KnownFolder(FOLDERID_Documents);
  if (docs.empty()) docs = KnownFolder(FOLDERID_Profile);
  return (fs::path(docs) / L"YCGrid").wstring();
}

std::string ShownPath(const std::wstring& path) {
  std::vector<core::PathRoot> roots;
  wchar_t buf[MAX_PATH + 1];
  if (const DWORD n = ::GetTempPathW(MAX_PATH + 1, buf); n > 0 && n <= MAX_PATH) roots.push_back({"%TEMP%", ToUtf8(buf)});
  for (const wchar_t* name : {L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE"}) {
    const DWORD n = ::GetEnvironmentVariableW(name, buf, MAX_PATH + 1);
    if (n > 0 && n <= MAX_PATH) roots.push_back({"%" + ToUtf8(name) + "%", ToUtf8(buf)});
  }
  return core::AbbreviatePath(ToUtf8(path), roots);
}

std::wstring AutoRecoverDir() {
  if (!g_settings.autorecover_folder.empty()) return g_settings.autorecover_folder;
  return (fs::path(KnownFolder(FOLDERID_LocalAppData)) / L"ycgrid" / L"AutoRecover").wstring();
}

}  // namespace yc::app
