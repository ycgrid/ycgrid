// SPDX-License-Identifier: Apache-2.0
// python_feeds.cpp - data feed processes: [[python.feeds]] in ycgrid.toml
//
//   [[python.feeds]]
//   name = "quotes"
//   script = "feeds/quotes.py"
//   restart = "always"      # or "never"
//   enabled = true
//
// Each feed runs in its own Python process (the project's runtime), started together with the Python host and stopped
// with it. The process gets YCGRID_PID, so yc.connect() connects to this YCGrid; its print() / errors show in the
// Immediate window. A feed that exits is restarted after 3 seconds (at most 5 times a minute) unless restart = "never".
#include "python_bridge.h"

#include "app_state.h"
#include "proc_util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace yc::app {

namespace fs = std::filesystem;

namespace {

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

// a TOML string or bare value without its quotes and trailing comment
std::string TomlValue(std::string v) {
  v = Trim(v);
  if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
    const char q = v[0];
    const size_t end = v.find(q, 1);
    return end == std::string::npos ? v.substr(1) : v.substr(1, end - 1);
  }
  const size_t hash = v.find('#');
  return Trim(hash == std::string::npos ? v : v.substr(0, hash));
}

struct FeedDef {
  std::string name, script, restart = "always";
  bool enabled = true;
};

std::vector<FeedDef> ReadFeeds(const fs::path& toml) {
  std::vector<FeedDef> out;
  std::ifstream f(toml);
  if (!f) return out;
  std::string line;
  bool in_feed = false;
  while (std::getline(f, line)) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == '#') continue;
    if (t[0] == '[') {
      in_feed = t == "[[python.feeds]]";
      if (in_feed) out.emplace_back();
      continue;
    }
    if (!in_feed) continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(t.substr(0, eq)), val = TomlValue(t.substr(eq + 1));
    FeedDef& d = out.back();
    if (key == "name") d.name = val;
    else if (key == "script") d.script = val;
    else if (key == "restart") d.restart = val;
    else if (key == "enabled") d.enabled = val != "false";
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

}  // namespace

void PythonBridge::StartFeeds() {
  StopFeeds();
  std::lock_guard<std::mutex> lk(feeds_mu_);
  const fs::path dir(app_dir_);
  std::error_code ec;
  if (dir.empty() || !fs::exists(dir / L"ycgrid.toml", ec)) return;
  const std::wstring ycdir = FindYcPackage();
  for (const FeedDef& d : ReadFeeds(dir / L"ycgrid.toml")) {
    if (!d.enabled || d.script.empty()) continue;
    fs::path script = fs::path(Widen(d.script));
    if (script.is_relative()) script = dir / script;
    if (!fs::exists(script, ec)) {
      Log("[YCGrid] Data feed \"" + d.name + "\": " + d.script + " was not found", 1);
      continue;
    }
    auto feed = std::make_unique<Feed>();
    feed->name = d.name.empty() ? script.stem().string() : d.name;
    feed->script = script.wstring();
    feed->restart = d.restart != "never";
    feed->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Feed* fp = feed.get();
    feed->runner = std::thread([this, fp, ycdir, app_dir = app_dir_] { RunFeed(fp, ycdir, app_dir); });
    feeds_.push_back(std::move(feed));
  }
}

void PythonBridge::RunFeed(Feed* f, const std::wstring& ycdir, const std::wstring& app_dir) {
  ::SetThreadDescription(::GetCurrentThread(), L"yc-feed");
  struct Done {
    Feed* f;
    ~Done() { f->done = true; }
  } done{f};
  std::vector<std::chrono::steady_clock::time_point> starts;
  for (;;) {
    // at most 5 starts a minute
    const auto now = std::chrono::steady_clock::now();
    starts.erase(std::remove_if(starts.begin(), starts.end(), [&](auto t) { return now - t > std::chrono::minutes(1); }), starts.end());
    if (starts.size() >= 5) {
      Log("[YCGrid] Data feed \"" + f->name + "\" keeps exiting; it is no longer restarted (Developer -> Reload starts it again)", 1);
      return;
    }
    starts.push_back(now);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    const std::wstring boot = L"import sys,runpy;sys.path[0:0]=[r'" + app_dir + L"',r'" + ycdir + L"'];sys.argv=[r'" + f->script +
                              L"'];runpy.run_path(r'" + f->script + L"',run_name='__main__')";
    std::wstring cmd = L"\"" + python_exe_ + L"\" -E -s -X utf8 -u -c \"" + boot + L"\"";
    std::wstring env;
    if (LPWCH e = ::GetEnvironmentStringsW()) {
      for (const wchar_t* p = e; *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        std::wstring up = kv.substr(0, kv.find(L'='));
        for (auto& ch : up) ch = static_cast<wchar_t>(::towupper(ch));
        if (up.rfind(L"PYTHON", 0) == 0 || up == L"VIRTUAL_ENV" || up.rfind(L"CONDA", 0) == 0 || up == L"YCGRID_PID" || up == L"YCGRID_FEED") continue;
        env += kv;
        env.push_back(L'\0');
      }
      ::FreeEnvironmentStringsW(e);
    }
    const std::wstring extra[] = {L"PYTHONIOENCODING=utf-8", L"YCGRID_PID=" + std::to_wstring(::GetCurrentProcessId()), L"YCGRID_FEED=" + Widen(f->name)};
    for (const std::wstring& kv : extra) {
      env += kv;
      env.push_back(L'\0');
    }
    env.push_back(L'\0');
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    // the feed inherits its own output pipe only (never another child's: that pipe would never end)
    const BOOL ok = CreateProcessInheriting(python_exe_.c_str(), buf.data(), {wr}, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                                            app_dir.c_str(), si, &pi);
    ::CloseHandle(wr);
    if (!ok) {
      ::CloseHandle(rd);
      Log("[YCGrid] Data feed \"" + f->name + "\" couldn't start (" + std::to_string(::GetLastError()) + ")", 1);
      return;
    }
    ::CloseHandle(pi.hThread);
    if (job_) ::AssignProcessToJobObject(job_, pi.hProcess);
    {
      std::lock_guard<std::mutex> lk(f->mu);
      f->process = pi.hProcess;
      // StopFeeds came while the process was starting: it couldn't stop it, so it goes now
      if (::WaitForSingleObject(f->stop_event, 0) == WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 0);
    }
    Log("[YCGrid] Data feed \"" + f->name + "\" started", 2);
    // its output, line by line, into the Immediate window
    std::string pending;
    char chunk[4096];
    DWORD n = 0;
    while (::ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
      pending.append(chunk, n);
      size_t nl;
      while ((nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pending.erase(0, nl + 1);
        const bool err = line.rfind("Traceback", 0) == 0 || line.rfind("  File ", 0) == 0 || line.find("Error:") != std::string::npos;
        Log("[" + f->name + "] " + line, err ? 1 : 0);
      }
    }
    ::CloseHandle(rd);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    {
      std::lock_guard<std::mutex> lk(f->mu);
      f->process = nullptr;
    }
    ::CloseHandle(pi.hProcess);
    if (::WaitForSingleObject(f->stop_event, 0) == WAIT_OBJECT_0) return;  // stopped by YCGrid
    Log("[YCGrid] Data feed \"" + f->name + "\" exited (code " + std::to_string(code) + ")" + (f->restart ? "; restarting in 3 seconds" : ""), code ? 1 : 2);
    if (!f->restart) return;
    if (::WaitForSingleObject(f->stop_event, 3000) == WAIT_OBJECT_0) return;
  }
}

void PythonBridge::StopFeeds() {
  std::lock_guard<std::mutex> lk(feeds_mu_);
  for (auto& f : feeds_) {
    std::lock_guard<std::mutex> fl(f->mu);
    ::SetEvent(f->stop_event);
    if (f->process) ::TerminateProcess(f->process, 0);
  }
  // never wait forever (the UI thread calls this): a runner still reading after the wait is woken up, and one that
  // doesn't end is left behind (its Feed stays allocated) rather than freezing YCGrid
  static std::vector<std::unique_ptr<Feed>> left_behind;
  for (auto& f : feeds_) {
    for (int i = 0; i < 100 && !f->done; ++i) {
      if (i == 50) {
        std::lock_guard<std::mutex> fl(f->mu);
        if (f->process) ::TerminateProcess(f->process, 0);
        if (f->runner.joinable()) ::CancelSynchronousIo(f->runner.native_handle());
      }
      ::Sleep(50);
    }
    if (f->done) {
      if (f->runner.joinable()) f->runner.join();
      ::CloseHandle(f->stop_event);
    } else {
      Log("[YCGrid] Data feed \"" + f->name + "\" didn't stop in 5 seconds; it is left to end on its own", 1);
      if (f->runner.joinable()) f->runner.detach();
      left_behind.push_back(std::move(f));
    }
  }
  feeds_.clear();
}

std::vector<std::string> PythonBridge::FeedNames() const {
  std::vector<std::string> out;
  for (const auto& f : feeds_) out.push_back(f->name);
  return out;
}

}  // namespace yc::app
