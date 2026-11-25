// SPDX-License-Identifier: Apache-2.0
// watchdog.cpp - [YC] hang and crash dumps (see watchdog.h)
#include "watchdog.h"

#include "error_log.h"

#include "core/i18n.h"

#include <crtdbg.h>
#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace yc::app {

namespace {

constexpr int kHangSeconds = 10;
constexpr int kMaxDumps = 3;

std::atomic<bool> g_stop{false};
std::thread g_thread;
std::atomic<int> g_dumps{0};
HWND g_hwnd = nullptr;

// A crash is written by a thread of its own, not by the thread that crashed: MiniDumpWriteDump called from the
// faulting thread is unreliable (Microsoft's documentation), and a crash in chaos round 5093 left an empty file.
std::thread g_writer;
HANDLE g_crash_request = nullptr, g_crash_done = nullptr;
std::atomic<DWORD> g_writer_tid{0};
EXCEPTION_POINTERS* g_crash_ex = nullptr;
DWORD g_crash_tid = 0;
const wchar_t* g_crash_kind = L"crash";
std::mutex g_write_mu;

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Write(const wchar_t* kind, EXCEPTION_POINTERS* ex, DWORD tid = ::GetCurrentThreadId()) {
  const std::wstring dir = DumpFolder();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  SYSTEMTIME t;
  ::GetLocalTime(&t);
  wchar_t name[96];
  swprintf_s(name, L"ycgrid-%s-%04d%02d%02d-%02d%02d%02d-%lu.dmp", kind, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             ::GetCurrentProcessId());
  const std::wstring path = dir + L"\\" + name;
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return {};
  MINIDUMP_EXCEPTION_INFORMATION mei{};
  mei.ThreadId = tid;
  mei.ExceptionPointers = ex;
  mei.ClientPointers = FALSE;
  const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);
  const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), f, type, ex ? &mei : nullptr, nullptr, nullptr);
  ::CloseHandle(f);
  if (!ok) ::DeleteFileW(path.c_str());  // an empty file says nothing and reads as a dump
  return ok ? path : std::wstring();
}

// the dump of this thread at `ex`, written by the writer thread; back once it is written
void WriteOnWriter(const wchar_t* kind, EXCEPTION_POINTERS* ex) {
  std::lock_guard<std::mutex> lk(g_write_mu);
  if (g_crash_request && g_writer_tid != 0 && g_writer_tid != ::GetCurrentThreadId()) {
    g_crash_kind = kind;
    g_crash_ex = ex;
    g_crash_tid = ::GetCurrentThreadId();
    ::ResetEvent(g_crash_done);
    ::SetEvent(g_crash_request);
    ::WaitForSingleObject(g_crash_done, 60000);
  } else {
    Write(kind, ex);
  }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ex) {
  // the error log may not be safe to use any more: just the file
  static std::atomic<bool> once{false};
  if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;  // a second thread crashing meanwhile
  WriteOnWriter(L"crash", ex);
  return EXCEPTION_CONTINUE_SEARCH;  // Windows Error Reporting goes on as before
}

#ifdef _DEBUG
// A debug build's failed check (an invalid std::clamp, a vector index) shows the CRT's dialog and nothing else: a
// dump of the thread at the check gives its stack (chaos round 5099 met one in a header drag and left no trace).
LONG AssertFilter(EXCEPTION_POINTERS* ex) {
  WriteOnWriter(L"assert", ex);
  return EXCEPTION_EXECUTE_HANDLER;
}

void DumpThisThread() {
  __try {
    ::RaiseException(0xE0590001, 0, 0, nullptr);
  } __except (AssertFilter(GetExceptionInformation())) {
  }
}

int __cdecl OnCrtReport(int type, char*, int*) {
  static std::atomic<int> n{0};
  if ((type == _CRT_ASSERT || type == _CRT_ERROR) && n++ < kMaxDumps) DumpThisThread();
  return FALSE;  // the report goes on as before (its dialog)
}
#endif

void WriterLoop() {
  g_writer_tid = ::GetCurrentThreadId();
  HANDLE waits[] = {g_crash_request};
  while (::WaitForMultipleObjects(1, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
    if (g_stop) break;
    Write(g_crash_kind, g_crash_ex, g_crash_tid);
    ::SetEvent(g_crash_done);
  }
}

void Loop() {
  int stalled = 0;
  std::chrono::steady_clock::time_point first_fail{};
  bool dumped = false;
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (g_stop || !g_hwnd) break;
    DWORD_PTR r = 0;
    // the UI thread answers WM_NULL as soon as it pumps messages (every frame)
    const bool alive = ::SendMessageTimeoutW(g_hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r) != 0;
    if (alive) {
      stalled = 0;
      dumped = false;
      continue;
    }
    if (::GetLastError() == ERROR_INVALID_WINDOW_HANDLE) break;  // closing
    if (stalled == 0) first_fail = std::chrono::steady_clock::now();
    // measured, not counted: with SMTO_ABORTIFHUNG a probe fails at once after the first few seconds
    stalled = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - first_fail).count()) + 1);
    if (stalled >= kHangSeconds && !dumped && g_dumps < kMaxDumps) {
      dumped = true;
      ++g_dumps;
      const std::wstring path = Write(L"hang", nullptr);
      ReportError("YCGrid", TR("The window stopped responding"),
                  TR("The window didn't answer for this many seconds: ") + std::to_string(stalled) + "\n" +
                      TR("A dump with every thread's stack was written to: ") + (path.empty() ? std::string("-") : Narrow(path)) + "\n" +
                      TR("Please send it with a description of what was running."));
    }
  }
}

}  // namespace

std::wstring DumpFolder() {
  std::wstring dir = ErrorLogFolder();
  if (dir.empty()) {
    wchar_t tmp[MAX_PATH];
    const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    dir = std::wstring(tmp, n) + L"ycgrid-dumps";
  }
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  return dir;
}

std::wstring WriteDumpNow(const char* why) {
  (void)why;
  return Write(L"manual", nullptr);
}

void StartWatchdog(HWND hwnd) {
  g_hwnd = hwnd;
  g_stop = false;
  g_crash_request = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_crash_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_writer = std::thread([] {
    ::SetThreadDescription(::GetCurrentThread(), L"yc-dump-writer");
    WriterLoop();
  });
  ::SetUnhandledExceptionFilter(OnCrash);
#ifdef _DEBUG
  _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, OnCrtReport);
#endif
  g_thread = std::thread([] {
    ::SetThreadDescription(::GetCurrentThread(), L"yc-watchdog");
    Loop();
  });
}

void StopWatchdog() {
  g_stop = true;
  if (g_thread.joinable()) g_thread.join();
  if (g_writer.joinable()) {
    g_writer_tid = 0;  // a crash from here on writes its own dump
    ::SetEvent(g_crash_request);
    g_writer.join();
  }
}

}  // namespace yc::app
