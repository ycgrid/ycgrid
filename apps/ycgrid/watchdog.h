// SPDX-License-Identifier: Apache-2.0
// watchdog.h - [YC] diagnostics for hangs and crashes
//
// A background thread asks the main window to answer (SendMessageTimeout with WM_NULL) every second. When the UI
// thread hasn't pumped messages for kHangSeconds it writes a minidump of the whole process (every thread's stack)
// next to the error log and reports it in the Error Log window, once per hang (at most a few per session). A crash
// writes a dump the same way. The dumps go to the error log folder, or %TEMP%\ycgrid-dumps when the log file is off
// (hidden instances).
#pragma once

#include <windows.h>

#include <string>

namespace yc::app {

void StartWatchdog(HWND hwnd);
void StopWatchdog();
std::wstring DumpFolder();
// write a minidump now (tests, Developer tools); returns its path or "" on failure
std::wstring WriteDumpNow(const char* why);

}  // namespace yc::app
