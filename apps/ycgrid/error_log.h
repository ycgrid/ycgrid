// SPDX-License-Identifier: Apache-2.0
// error_log.h - every error YCGrid shows or receives, in one place: the Error Log window (View > Error Log), the status bar's
// error count, Python (yc.ui.errors) and a daily log file (settings.json: errors.log_to_file, errors.log_folder).
#pragma once

#include "io/msgvalue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yc::app {

struct AppState;

struct ErrorEntry {
  int64_t id = 0;
  double time = 0;     // Unix seconds
  std::string source;  // "YCGrid", "Python", "VS Code"...
  std::string title;
  std::string text;
  bool error = true;  // false: a message YCGrid showed (kept with the errors, filtered out by "Errors only")
};

// any thread
void ReportError(const std::string& source, const std::string& title, const std::string& text, bool error = true);
// pieces of an error stream (Python's stderr): pieces within 0.3 s of each other become one entry
void ReportErrorStream(const std::string& source, const std::string& text);
std::vector<ErrorEntry> ErrorEntries(int64_t after_id = 0);
int ErrorsUnseen();    // errors (not messages) since the Errors window was last shown
int MessagesUnseen();  // messages (not errors) since then: the status bar's Error Log entry shows them as new
void ClearErrors();
// errors that no longer apply (a missing module installed since): they become messages, so the status bar's
// error count drops, and keep their text with the note added; returns how many changed
int ResolveErrors(const std::vector<int64_t>& ids, const std::string& note);
std::wstring ErrorLogFolder();  // where the files go ("" = writing is off)
void EnableErrorLogFile(bool on);  // off for hidden instances (tests, agents) unless settings say otherwise

// UI thread
void RegisterErrorCommands(AppState& s);
void DrawErrorWindow(AppState& s);
void OpenErrorWindow();
io::MV ErrorRequest(const std::string& m, const io::MV& p, std::string* err);  // errors.list / errors.clear / errors.resolve

}  // namespace yc::app
