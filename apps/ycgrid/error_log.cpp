// SPDX-License-Identifier: Apache-2.0
// error_log.cpp - the error log: entries in memory (the newest 2,000), the Errors window, and a daily file.
#include "error_log.h"

#include "app_state.h"
#include "core/i18n.h"
#include "settings.h"
#include "python_bridge.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "theme/theme.h"

#include "imgui.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace yc::app {

using io::MV;
namespace fs = std::filesystem;

namespace {

std::mutex g_mu;
std::deque<ErrorEntry> g_entries;
int64_t g_next = 1;
int64_t g_seen = 0;           // the newest entry shown in the window
double g_stream_at = -1;      // when the last stream piece came (merging)
std::string g_stream_source;  // and from whom
bool g_file = true;
bool g_show = false;
bool g_errors_only = false;
int64_t g_text_for = -1;  // the text below was built up to this entry
bool g_text_errors_only = false;
std::string g_text;

double Now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Stamp(double t, bool date) {
  const std::time_t secs = static_cast<std::time_t>(t);
  std::tm tm{};
  localtime_s(&tm, &secs);
  char buf[64];
  const int ms = static_cast<int>((t - static_cast<double>(secs)) * 1000);
  if (date) {
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, ms);
  } else {
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  }
  return buf;
}

std::wstring DefaultFolder() {
  wchar_t* local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) dir = std::wstring(local) + L"\\ycgrid\\logs";
  if (local) ::CoTaskMemFree(local);
  return dir;
}

std::string Indented(const std::string& text) {
  std::string out;
  size_t a = 0;
  while (a <= text.size()) {
    const size_t b = text.find('\n', a);
    const std::string line = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (!line.empty() && line != "\r") out += "    " + line + "\n";
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

std::string EntryText(const ErrorEntry& e, bool date) {
  std::string s = Stamp(e.time, date) + (e.error ? "  ERROR  [" : "  message  [") + e.source + "]  " + e.title + "\n";
  return s + Indented(e.text);
}

// one file a day: errors-2026-10-25.log; files older than 30 days are removed when YCGrid starts
void AppendToFile(const ErrorEntry& e) {
  const std::wstring dir = ErrorLogFolder();
  if (dir.empty()) return;
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string day = Stamp(e.time, true).substr(0, 10);
  std::ofstream f(fs::path(dir) / ("errors-" + day + ".log"), std::ios::binary | std::ios::app);
  if (f) f << EntryText(e, true) << "\n";
}

void Add(ErrorEntry e) {
  e.id = g_next++;
  AppendToFile(e);  // nothing when ErrorLogFolder() is ""
  g_entries.push_back(std::move(e));
  while (g_entries.size() > 2000) g_entries.pop_front();
}

}  // namespace

void ReportError(const std::string& source, const std::string& title, const std::string& text, bool error) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_stream_at = -1;
  Add(ErrorEntry{0, Now(), source, title, text, error});
}

void ReportErrorStream(const std::string& source, const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const double t = Now();
  // a piece that starts a new error (a traceback, "[where] Error: ...", "Exception in thread ...") is its own entry
  const bool starts_new = text.rfind("Traceback", 0) == 0 || text.rfind("[", 0) == 0 || text.rfind("Exception", 0) == 0;
  if (!starts_new && !g_entries.empty() && g_stream_source == source && g_stream_at >= 0 && t - g_stream_at < 0.3) {
    // the same burst (a traceback written line by line): one entry; the file gets the piece as a continuation
    ErrorEntry& last = g_entries.back();
    last.text += (last.text.empty() || last.text.back() == '\n' ? "" : "\n") + text;
    if (!ErrorLogFolder().empty()) {
      const std::string day = Stamp(last.time, true).substr(0, 10);
      std::ofstream f(fs::path(ErrorLogFolder()) / ("errors-" + day + ".log"), std::ios::binary | std::ios::app);
      if (f) f << Indented(text) << "\n";
    }
  } else {
    // the first line names it; the rest is the detail
    const size_t nl = text.find('\n');
    Add(ErrorEntry{0, t, source, text.substr(0, nl), nl == std::string::npos ? std::string() : text.substr(nl + 1), true});
  }
  g_stream_at = t;
  g_stream_source = source;
}

std::vector<ErrorEntry> ErrorEntries(int64_t after_id) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<ErrorEntry> out;
  for (const auto& e : g_entries) {
    if (e.id > after_id) out.push_back(e);
  }
  return out;
}

int ErrorsUnseen() {
  std::lock_guard<std::mutex> lk(g_mu);
  int n = 0;
  for (auto it = g_entries.rbegin(); it != g_entries.rend() && it->id > g_seen; ++it) n += it->error ? 1 : 0;
  return n;
}

int MessagesUnseen() {
  std::lock_guard<std::mutex> lk(g_mu);
  int n = 0;
  for (auto it = g_entries.rbegin(); it != g_entries.rend() && it->id > g_seen; ++it) n += it->error ? 0 : 1;
  return n;
}

int ResolveErrors(const std::vector<int64_t>& ids, const std::string& note) {
  std::lock_guard<std::mutex> lk(g_mu);
  int n = 0;
  for (auto& e : g_entries) {
    if (!e.error || std::find(ids.begin(), ids.end(), e.id) == ids.end()) continue;
    e.error = false;
    if (!note.empty()) e.text += std::string(e.text.empty() || e.text.back() == '\n' ? "" : "\n") + note;
    ++n;
  }
  if (n) g_text_for = -1;  // the window's text is built again
  return n;
}

void ClearErrors() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_entries.clear();
  g_seen = g_next - 1;
  g_text_for = -1;
}

// YCGRID_ERROR_LOG=<folder> writes the log there, hidden instances included (tests, CI)
std::wstring EnvFolder() {
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = ::GetEnvironmentVariableW(L"YCGRID_ERROR_LOG", buf, static_cast<DWORD>(std::size(buf)));
  return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

int AppMessageBox(AppState& s, const std::string& text, const std::string& title, unsigned flags, int auto_answer) {
  if (s.headless || (s.python && s.python->AutoDialogs())) {
    if (s.python) s.python->LogLine("[MsgBox] " + text, 2);
    ReportError("YCGrid", title.empty() ? std::string("YCGrid") : title,
                text + "\n(" + TR("answered automatically: nobody can see this window") + ")", (flags & 0xF0) == MB_ICONERROR);
    return auto_answer;
  }
  return ::MessageBoxW(s.hwnd, FromUtf8(text).c_str(), FromUtf8(title.empty() ? std::string("YCGrid") : title).c_str(), flags);
}

namespace {
std::mutex g_answers_mu;
std::deque<std::wstring> g_answers;  // file dialog answers queued by tests
}  // namespace

void QueueFileDialogAnswers(const std::vector<std::wstring>& paths) {
  std::lock_guard<std::mutex> lk(g_answers_mu);
  g_answers.insert(g_answers.end(), paths.begin(), paths.end());
}

bool NoFileDialog(HWND owner, const std::string& what, std::wstring* answer) {
  if (owner && ::IsWindowVisible(owner)) return false;
  if (answer) {
    answer->clear();
    std::lock_guard<std::mutex> lk(g_answers_mu);
    if (!g_answers.empty()) {
      *answer = g_answers.front();
      g_answers.pop_front();
      if (!answer->empty()) {
        ReportError("YCGrid", what, std::string(TR("The file dialog chose ")) + ToUtf8(*answer) + "\n(" + TR("answered automatically: a test queued the answer") + ")", false);
        return true;
      }
    }
  }
  ReportError("YCGrid", what, std::string(TR("The file dialog was cancelled")) + "\n(" + TR("answered automatically: nobody can see this window") + ")", false);
  return true;
}

std::wstring ErrorLogFolder() {
  if (const std::wstring env = EnvFolder(); !env.empty()) return env;
  if (!g_file || !Config().errors_log_to_file) return {};
  return Config().errors_log_folder.empty() ? DefaultFolder() : Config().errors_log_folder;
}

void EnableErrorLogFile(bool on) {
  g_file = on;
  const std::wstring dir = ErrorLogFolder();
  if (dir.empty()) return;
  // keep 30 days of files
  std::error_code ec;
  const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    const std::wstring name = f.path().filename().wstring();
    if (name.rfind(L"errors-", 0) == 0 && f.path().extension() == L".log" && f.last_write_time(ec) < limit) fs::remove(f.path(), ec);
  }
}

void OpenErrorWindow() { g_show = true; }

void RegisterErrorCommands(AppState&) {
  ui::Command c;
  c.id = "ErrorLog";
  c.label = TR("Error Log");
  c.icon = ui::icon::Warning;
  c.tip_desc = TR("Every error YCGrid showed or received (Python included), newest first; also written to a daily log file.");
  ui::Commands().Add(c).execute = [](ui::Command&) { g_show = !g_show; };
  RegisterPane("errors", "ErrorLog", []() -> bool& { return g_show; });
}

void DrawErrorWindow(AppState& s) {
  if (!g_show) return;
  using ui::S;
  ImGui::SetNextWindowSize(ImVec2(S(780), S(440)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Center);
  if (ImGui::Begin(TR("Error Log###yc_errors"), &g_show)) {
    std::vector<ErrorEntry> all = ErrorEntries();
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_seen = g_next - 1;  // shown: the status bar count goes away
    }
    ImGui::Checkbox(TR("Errors only"), &g_errors_only);
    ImGui::SameLine();
    const int64_t last = all.empty() ? 0 : all.back().id;
    if (last != g_text_for || g_errors_only != g_text_errors_only) {
      g_text.clear();
      for (auto it = all.rbegin(); it != all.rend(); ++it) {  // newest first
        if (!g_errors_only || it->error) g_text += EntryText(*it, true) + "\n";
      }
      g_text_for = last;
      g_text_errors_only = g_errors_only;
    }
    if (ImGui::Button(TR("Copy All"))) {
      ImGui::SetClipboardText(g_text.c_str());
      s.SetStatus(TR("Copied the errors"));
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"))) {
      ClearErrors();
      g_text.clear();
    }
    const std::wstring folder = ErrorLogFolder();
    if (!folder.empty()) {
      ImGui::SameLine();
      if (ImGui::Button(TR("Open Log Folder"))) {
        std::error_code ec;
        fs::create_directories(folder, ec);
        OpenOnDesktop(folder);  // a hidden instance opens no Explorer window (the folder is shown beside the button)
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", ToUtf8(folder).c_str());
    } else {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", TR("(not written to a file: settings.json errors.log_to_file)"));
    }
    if (g_text.empty()) {
      ImGui::TextDisabled("%s", TR("No errors."));
    } else {
      // read-only text: select any part and copy it (Ctrl+A, Ctrl+C)
      ImGui::InputTextMultiline("##errors", g_text.data(), g_text.size() + 1, ImVec2(-1, -1),
                                ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    }
  }
  ImGui::End();
}

io::MV ErrorRequest(const std::string& m, const io::MV& p, std::string* err) {
  if (m == "errors.list") {
    MV a = MV::Array();
    const bool only = p.GetBool("errors_only", false);
    for (const auto& e : ErrorEntries(p.GetInt("after", 0))) {
      if (only && !e.error) continue;
      MV x = MV::Map();
      x.Set("id", MV::Int(e.id));
      x.Set("time", MV::Double(e.time));
      x.Set("source", MV::Str(e.source));
      x.Set("title", MV::Str(e.title));
      x.Set("text", MV::Str(e.text));
      x.Set("error", MV::Bool(e.error));
      a.Push(std::move(x));
    }
    return a;
  }
  if (m == "errors.clear") {
    ClearErrors();
    return MV::Bool(true);
  }
  if (m == "errors.resolve") {
    std::vector<int64_t> ids;
    if (const MV* a = p.Get("ids")) {
      for (const MV& x : a->a) ids.push_back(x.i);
    }
    return MV::Int(ResolveErrors(ids, p.GetStr("note")));
  }
  if (m == "errors.folder") return MV::Str(ToUtf8(ErrorLogFolder()));
  *err = "Unknown request: " + m;
  return MV();
}

}  // namespace yc::app
