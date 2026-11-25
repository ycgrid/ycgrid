// SPDX-License-Identifier: Apache-2.0
// windows_ui.cpp - View > Window: New Window, Split (sheet_actions.cpp), Switch Windows, Arrange All; each further workbook opens
// in its own YCGrid window (like Excel since 2013)
#include "app_state.h"
#include "settings.h"
#include "core/i18n.h"

#include "trust.h"
#include "app/shell_integration.h"
#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <shellapi.h>

#include "io/msgpack.h"
#include "io/msgvalue.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <future>

namespace yc::app {

namespace fs = std::filesystem;

namespace {

struct OtherWindow {
  DWORD pid = 0;
  HWND hwnd = nullptr;
  std::wstring title;
  std::wstring file;
  std::string pipe;  // its control pipe (YCP)
};

// the other running YCGrid windows (their instance files list the pid; the window is found by pid and class)
// need_window false: also the instances without a window on the screen (hidden, automated)
std::vector<OtherWindow> OtherWindows(bool need_window = true) {
  std::vector<OtherWindow> out;
  std::error_code ec;
  wchar_t base[MAX_PATH] = {};
  ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  const fs::path dir = fs::path(base) / L"ycgrid" / L"instances";
  if (!fs::exists(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != L".json") continue;
    nlohmann::json j;
    try {
      std::ifstream f(e.path(), std::ios::binary);
      f >> j;
    } catch (...) {
      continue;
    }
    OtherWindow w;
    w.pid = j.value("pid", 0u);
    if (!w.pid || w.pid == ::GetCurrentProcessId()) continue;
    // the file of a YCGrid that ended without removing it (killed, crashed): removed, so the list stays short
    HANDLE ph = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, w.pid);
    DWORD code = 0;
    const bool alive = ph && ::GetExitCodeProcess(ph, &code) && code == STILL_ACTIVE;
    if (ph) ::CloseHandle(ph);
    if (!alive) {
      fs::remove(e.path(), ec);
      continue;
    }
    w.file = FromUtf8(j.value("file", std::string()));
    w.pipe = j.value("pipe", std::string());
    struct Ctx {
      DWORD pid;
      HWND found;
    } ctx{w.pid, nullptr};
    ::EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
          auto* c = reinterpret_cast<Ctx*>(lp);
          DWORD p = 0;
          ::GetWindowThreadProcessId(h, &p);
          if (p == c->pid && ::IsWindowVisible(h) && !::GetWindow(h, GW_OWNER)) {
            wchar_t cls[64];
            if (::GetClassNameW(h, cls, 64) && wcscmp(cls, app::shell::kWindowClass) == 0) {
              c->found = h;
              return FALSE;
            }
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found && need_window) continue;  // a stale file
    w.hwnd = ctx.found;
    wchar_t t[512] = {};
    if (w.hwnd) ::GetWindowTextW(w.hwnd, t, 512);
    w.title = t;
    out.push_back(std::move(w));
  }
  std::sort(out.begin(), out.end(), [](const OtherWindow& a, const OtherWindow& b) { return a.title < b.title; });
  return out;
}

void Activate(HWND h) {
  if (::IsIconic(h)) ::ShowWindow(h, SW_RESTORE);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  ::AllowSetForegroundWindow(pid);
  ::SetForegroundWindow(h);
}

// ---- View Side by Side with another YCGrid (another workbook): its YCP pipe is told what happens here ----
struct Partner {
  DWORD pid = 0;          // the other YCGrid shown side by side (0 = none)
  std::wstring pipe;
  std::string with;       // CompareSideBySideWith(WindowName): the workbook asked for (one use)
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::pair<std::string, io::MV>> queue;  // requests to send, in order
  bool thread_started = false;
  std::atomic<bool> failed{false};
};
Partner& P() {
  static Partner p;
  return p;
}
// the scroll position the windows side by side were last seen at (a change is sent / taken along)
int64_t g_last_v = -1, g_last_h = -1;
uint32_t g_last_focus = UINT32_MAX;

void SenderLoop() {
  Partner& p = P();
  HANDLE h = INVALID_HANDLE_VALUE;
  std::wstring connected;
  int64_t next_id = 1;
  for (;;) {
    std::pair<std::string, io::MV> item;
    std::wstring pipe;
    {
      std::unique_lock<std::mutex> lk(p.mu);
      p.cv.wait(lk, [&] { return !p.queue.empty(); });
      item = std::move(p.queue.front());
      p.queue.pop_front();
      pipe = p.pipe;
    }
    if (h != INVALID_HANDLE_VALUE && connected != pipe) {
      ::CloseHandle(h);
      h = INVALID_HANDLE_VALUE;
    }
    if (h == INVALID_HANDLE_VALUE && !pipe.empty()) {
      for (int attempt = 0; attempt < 2 && h == INVALID_HANDLE_VALUE; ++attempt) {
        h = ::CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) ::WaitNamedPipeW(pipe.c_str(), 300);
      }
      connected = pipe;
    }
    if (h == INVALID_HANDLE_VALUE) {
      p.failed = true;
      continue;
    }
    // YCP: a frame (u32 length, msgpack [0, id, method, params]); the answer is read and dropped
    io::MsgWriter w;
    w.Array(4);
    w.Int(0);
    w.Int(next_id++);
    w.Str(item.first);
    io::Encode(w, item.second);
    const std::string& data = w.buf();
    const uint32_t len = static_cast<uint32_t>(data.size());
    std::string frame(4, '\0');
    std::memcpy(frame.data(), &len, 4);
    frame += data;
    DWORD done = 0;
    bool ok = ::WriteFile(h, frame.data(), static_cast<DWORD>(frame.size()), &done, nullptr) && done == frame.size();
    uint32_t rlen = 0;
    DWORD got = 0;
    ok = ok && ::ReadFile(h, &rlen, 4, &got, nullptr) && got == 4 && rlen < (1u << 24);
    std::string reply(rlen, '\0');
    for (DWORD at = 0; ok && at < rlen; at += got) ok = ::ReadFile(h, reply.data() + at, rlen - at, &got, nullptr) && got > 0;
    if (!ok) {
      ::CloseHandle(h);
      h = INVALID_HANDLE_VALUE;
      p.failed = true;
    }
  }
}

// a request to the partner, sent by the worker thread (scrolls in a row are merged into one)
void SendToPartner(const std::string& method, io::MV params) {
  Partner& p = P();
  std::lock_guard<std::mutex> lk(p.mu);
  if (method == "layout.sync_scroll" && !p.queue.empty() && p.queue.back().first == method) {
    io::MV& last = p.queue.back().second;
    last.Set("dv", io::MV::Int(last.GetInt("dv") + params.GetInt("dv")));
    last.Set("dh", io::MV::Int(last.GetInt("dh") + params.GetInt("dh")));
  } else {
    p.queue.emplace_back(method, std::move(params));
  }
  if (!p.thread_started) {
    p.thread_started = true;
    std::thread(SenderLoop).detach();
  }
  p.cv.notify_one();
}

constexpr int kSwitchSlots = 16;
struct SwitchEntry {
  std::string label;
  uint32_t extra = 0;  // an extra window of this workbook (0 = the main window)
  HWND other = nullptr;
};
std::vector<SwitchEntry> g_switch;

}  // namespace

std::vector<OtherWorkbook> OtherWorkbooks(bool hidden_too) {
  std::vector<OtherWorkbook> out;
  for (const OtherWindow& w : OtherWindows(!hidden_too)) {
    OtherWorkbook b;
    b.name = ToUtf8(w.file.empty() ? w.title : fs::path(w.file).filename().wstring());
    b.file = w.file;
    b.pipe = w.pipe;
    out.push_back(std::move(b));
  }
  return out;
}

bool WorkbookInUse(const AppState& s) {
  if (!s.snap) return false;
  if (!s.file_path.empty() || s.snap->wb.dirty) return true;
  for (const auto& sh : s.snap->wb.sheets) {
    if (sh->has_used) return true;
  }
  return false;
}

bool ActivateWindowWithFile(const std::wstring& path) {
  for (const auto& w : OtherWindows()) {
    std::error_code ec;
    if (!w.file.empty() && fs::equivalent(w.file, path, ec)) {
      Activate(w.hwnd);
      return true;
    }
  }
  return false;
}

bool OpenInNewWindow(const std::wstring& path, const std::string& password, bool as_new, uint32_t* pid) {
  if (pid) *pid = 0;  // the process started, so a script works with that window and no other (Workbooks.Add)
  if (!path.empty() && ActivateWindowWithFile(path)) return true;
  const std::wstring exe = app::shell::ExePath();
  std::wstring cmd = L"\"" + exe + L"\" --new-window";
  if (!UserStateSaved()) {
    // a hidden instance (tests, agents) opens hidden windows that end with it: nothing appears on the user's screen
    cmd += L" --hidden --parent-pid " + std::to_wstring(::GetCurrentProcessId());
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
      for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--lang") cmd += L" --lang " + std::wstring(argv[i + 1]);
      }
      ::LocalFree(argv);
    }
  }
  cmd += trust::ChildArgs();  // a script's window (hidden, Workbooks.Open): its workbook follows AutomationSecurity too
  if (as_new) cmd += L" --as-new";
  if (!path.empty()) cmd += L" \"" + path + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = fs::path(exe).parent_path().wstring();
  // a password (Workbooks.Open(..., Password)) goes to the new window in its environment, not on the command line
  if (!password.empty()) ::SetEnvironmentVariableW(L"YCGRID_OPEN_PASSWORD", FromUtf8(password).c_str());
  const BOOL started = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi);
  if (!password.empty()) ::SetEnvironmentVariableW(L"YCGRID_OPEN_PASSWORD", nullptr);
  if (!started) return false;
  ::AllowSetForegroundWindow(pi.dwProcessId);
  if (pid) *pid = pi.dwProcessId;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

void NewWindow(AppState& s) {
  AppState::ExtraWin w;
  w.id = s.next_extra_win++;
  w.view = s.FocusView();
  w.view.sheet = s.focus_sheet;
  w.view.dragging = w.view.fill_dragging = w.view.move_dragging = false;
  s.extra_wins.push_back(std::move(w));
  s.focus_win = s.extra_wins.back().id;
}

// the other YCGrid windows with this one, placed as Arrange Windows does (0 Tiled, 1 Horizontal, 2 Vertical, 3 Cascade);
// never another program's window, and nothing for a hidden instance or a single window
void ArrangeOsWindows(AppState& s, int mode) {
  if (!s.hwnd || !::IsWindowVisible(s.hwnd)) return;
  std::vector<HWND> hs;
  hs.push_back(s.hwnd);
  for (const auto& w : OtherWindows()) hs.push_back(w.hwnd);
  if (hs.size() < 2) return;
  MONITORINFO mi{sizeof(mi)};
  ::GetMonitorInfoW(::MonitorFromWindow(hs[0], MONITOR_DEFAULTTONEAREST), &mi);
  const RECT wa = mi.rcWork;
  const int n = static_cast<int>(hs.size()), W = wa.right - wa.left, H = wa.bottom - wa.top;
  for (int i = 0; i < n; ++i) {
    HWND h = hs[static_cast<size_t>(i)];
    if (::IsZoomed(h) || ::IsIconic(h)) ::ShowWindow(h, SW_RESTORE);
    int x = wa.left, y = wa.top, w = W, hh = H;
    if (mode == 0) {
      const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))), rows = (n + cols - 1) / cols;
      w = W / cols, hh = H / rows, x += (i % cols) * w, y += (i / cols) * hh;
    } else if (mode == 1) {
      hh = H / n, y += i * hh;
    } else if (mode == 2) {
      w = W / n, x += i * w;
    } else {
      const int step = 30;
      w = W * 7 / 10, hh = H * 7 / 10, x += i * step, y += i * step;
    }
    ::SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

// View > Arrange All: Excel's Arrange Windows dialog
void DrawArrangeDialog(AppState& s) {
  static int mode = 0;
  static bool active_only = false;
  if (s.dialog != Dialog::ArrangeWindows) return;
  const char* title = TR("Arrange Windows###yc_dlg_arrange");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return;
  }
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::TextUnformatted(TR("Arrange"));
  ImGui::RadioButton(TR("Tiled"), &mode, 0);
  ImGui::RadioButton(TR("Horizontal"), &mode, 1);
  ImGui::RadioButton(TR("Vertical"), &mode, 2);
  ImGui::RadioButton(TR("Cascade"), &mode, 3);
  ImGui::Checkbox(TR("Windows of active workbook"), &active_only);
  ImGui::Separator();
  if (DlgOkButton(TR("OK"), ImVec2(ui::S(80), 0))) {
    s.arrange_mode = mode;
    s.arrange_windows = true;  // this workbook's windows (View > New Window), over the sheets
    if (!active_only) ArrangeOsWindows(s, mode);
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(ui::S(80), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// View > Unhide: the hidden windows of this workbook to show again
void DrawUnhideWindowDialog(AppState& s) {
  static int sel = 0;
  if (s.dialog != Dialog::UnhideWindow) return;
  const char* title = TR("Unhide###yc_dlg_unhidewin");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
    sel = 0;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return;
  }
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  const std::string book = s.file_path.empty() ? std::string("Book1") : ToUtf8(fs::path(s.file_path).filename().wstring());
  std::vector<std::pair<std::string, uint32_t>> hidden;  // (name, window: 0 = the first one)
  if (s.main_hidden) hidden.push_back({s.extra_wins.empty() ? book : book + ":1", 0});
  for (const auto& w : s.extra_wins) {
    if (w.hidden) hidden.push_back({book + ":" + std::to_string(w.id), w.id});
  }
  ImGui::TextUnformatted(TR("Unhide workbook:"));
  if (ImGui::BeginListBox("##hidden", ImVec2(ui::S(260), ui::S(120)))) {
    for (int i = 0; i < static_cast<int>(hidden.size()); ++i) {
      if (ImGui::Selectable(hidden[static_cast<size_t>(i)].first.c_str(), sel == i)) sel = i;
    }
    ImGui::EndListBox();
  }
  if (DlgOkButton(TR("OK"), ImVec2(ui::S(80), 0)) && sel >= 0 && sel < static_cast<int>(hidden.size())) {
    const uint32_t win = hidden[static_cast<size_t>(sel)].second;
    if (win == 0) {
      s.main_hidden = false;
      s.focus_win = 0;
      s.focus_sheet = s.active_sheet;
    }
    for (auto& w : s.extra_wins) {
      if (w.id == win) {
        w.hidden = false;
        w.focus_request = true;
        s.focus_win = w.id;
        s.focus_sheet = w.view.sheet;
      }
    }
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(ui::S(80), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

void SetSideBySideWith(const std::string& name) { P().with = name; }

DWORD SideBySidePartner() { return P().pid; }

void PartnerSideBySide(AppState& s, DWORD pid, bool on) {
  // the other YCGrid started or ended View Side by Side with this one: both windows scroll together
  Partner& p = P();
  if (on) {
    for (const auto& w : OtherWindows(false)) {
      if (w.pid == pid && !w.pipe.empty()) {
        std::lock_guard<std::mutex> lk(p.mu);
        p.pipe = FromUtf8(w.pipe);
      }
    }
    p.pid = pid;
    p.failed = false;
    s.side_by_side = true;
  } else if (p.pid == pid) {
    p.pid = 0;
    s.side_by_side = false;
  }
  g_last_v = g_last_h = -1;
}

void PartnerScrolled(AppState& s, int64_t dv, int64_t dh) {
  // the other YCGrid scrolled: this one follows, and takes the new place as its own (nothing is sent back)
  if (!s.side_by_side || !s.sync_scrolling) return;
  render::GridView& v = s.wins[s.active_sheet].view;
  v.v.first = std::max<int64_t>(0, v.v.first + dv);
  v.h.first = std::max<int64_t>(0, v.h.first + dh);
  g_last_v = v.v.first;
  g_last_h = v.h.first;
}

void DrawWindowDialogs(AppState& s) {
  DrawArrangeDialog(s);
  DrawUnhideWindowDialog(s);
}

void RegisterWindowCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon = nullptr, const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("NewWindow", TR("New\nWindow"), ui::icon::NewWindow, TR("Open a second window on this workbook, to work in different places at once."))).execute =
      [&s](ui::Command&) { NewWindow(s); };
  R.Add(make("ArrangeAll", TR("Arrange\nAll"), ui::icon::ArrangeAll, TR("Arrange the open windows: tiled, horizontal, vertical or cascade."))).execute =
      [&s](ui::Command&) { s.OpenDialog(Dialog::ArrangeWindows); };
  // ---- View > Window: Hide / Unhide, View Side by Side, Synchronous Scrolling, Reset Window Position ----
  R.Add(make("HideWindow", TR("Hide"), ui::icon::HideWindow, TR("Hide the current window."))).execute = [&s](ui::Command&) {
    // an entry in progress is written first, as when another sheet is clicked: left open in a hidden window, no
    // Escape reached it (basics.py --tab others, typing:cmd:HideWindow)
    if (s.edit.active) CommitEdit(s, false, 0);
    if (s.focus_win) {
      for (auto& w : s.extra_wins) {
        if (w.id == s.focus_win) w.hidden = true;
      }
      s.focus_win = 0;
      s.focus_sheet = s.active_sheet;
    } else {
      s.main_hidden = true;
    }
  };
  R.Add(make("UnhideWindow", TR("Unhide"), ui::icon::Eye, TR("Show a window you hid."))).execute = [&s](ui::Command&) {
    s.OpenDialog(Dialog::UnhideWindow);
  };
  R.Add(make("ViewSideBySide", TR("View Side by Side"), ui::icon::SideBySide, TR("Show two windows one above the other to compare them."))).execute =
      [&s](ui::Command&) {
        Partner& p = P();
        s.side_by_side = !s.side_by_side;
        if (!s.side_by_side) {
          // the other YCGrid stops too
          if (p.pid) {
            io::MV m = io::MV::Map();
            m.Set("on", io::MV::Bool(false));
            m.Set("pid", io::MV::Int(::GetCurrentProcessId()));
            SendToPartner("layout.side_by_side", std::move(m));
          }
          p.pid = 0;
          return;
        }
        // the window to compare with: the workbook asked for (CompareSideBySideWith), else this workbook's second
        // window, else the other YCGrid window (Excel's Compare Side by Side), else a new window of this workbook
        const std::string with = std::move(p.with);
        p.with.clear();
        const OtherWindow* other = nullptr;
        const std::vector<OtherWindow> others = OtherWindows(with.empty());
        for (const auto& w : others) {
          const std::string file = ToUtf8(fs::path(w.file).filename().wstring()), stem = ToUtf8(fs::path(w.file).stem().wstring());
          if (!with.empty() && (with == file || with == stem || ToUtf8(w.title).find(with) != std::string::npos)) other = &w;
        }
        if (with.empty() && s.extra_wins.empty() && !others.empty()) other = &others.front();
        if (other && !other->pipe.empty()) {
          p.pid = other->pid;
          {
            std::lock_guard<std::mutex> lk(p.mu);
            p.pipe = FromUtf8(other->pipe);
          }
          p.failed = false;
          io::MV m = io::MV::Map();
          m.Set("on", io::MV::Bool(true));
          m.Set("pid", io::MV::Int(::GetCurrentProcessId()));
          SendToPartner("layout.side_by_side", std::move(m));
          g_last_v = g_last_h = -1;
        } else if (s.extra_wins.empty()) {
          NewWindow(s);
        }
        s.arrange_mode = 1;
        s.arrange_windows = true;
        ArrangeOsWindows(s, 1);
      };
  R.Add(make("SyncScrolling", TR("Synchronous Scrolling"), ui::icon::SyncScroll, TR("Scroll the two windows side by side together."))).execute =
      [&s](ui::Command&) { s.sync_scrolling = !s.sync_scrolling; };
  R.Add(make("ResetWindowPosition", TR("Reset Window Position"), ui::icon::WindowPosition, TR("Give the windows side by side the same size again."))).execute =
      [&s](ui::Command&) {
        s.arrange_mode = 1;
        s.arrange_windows = true;
        ArrangeOsWindows(s, 1);
      };
  // View > Focus Cell: the active cell's row and column tinted, so the eye finds it in a large sheet (Excel)
  R.Add(make("FocusCell", TR("Focus\nCell"), ui::icon::FocusCell, TR("Highlight the row and column of the active cell."))).execute = [&s](ui::Command& c) {
    s.focus_cell = !s.focus_cell;
    c.toggle = true;
    c.pressed = s.focus_cell;
  };
  R.Add(make("SwitchWindowsMenu", TR("Switch\nWindows"), ui::icon::SwitchWindows, TR("Switch to another open window."))).execute = [](ui::Command&) {};
  // Ctrl+F6 / Ctrl+Tab and with Shift: the next / previous window of Switch Windows' list (Excel)
  auto step = [&s](int by) {
    const int n = static_cast<int>(g_switch.size());
    if (n < 2) return;
    int cur = 0;
    for (int i = 0; i < n; ++i) {
      if (g_switch[static_cast<size_t>(i)].extra && g_switch[static_cast<size_t>(i)].extra == s.focus_win) cur = i;
    }
    ui::Commands().Execute("SwitchWindow" + std::to_string(((cur + by) % n + n) % n));
  };
  R.Add(make("NextWindow", TR("Next Window"), ui::icon::SwitchWindows, TR("Switch to the next window (Ctrl+F6)."))).execute = [step](ui::Command&) { step(1); };
  R.Add(make("PrevWindow", TR("Previous Window"), ui::icon::SwitchWindows, TR("Switch to the previous window (Ctrl+Shift+F6)."))).execute = [step](ui::Command&) { step(-1); };
  for (int i = 0; i < kSwitchSlots; ++i) {
    const std::string id = "SwitchWindow" + std::to_string(i);
    R.Add(make(id.c_str(), "")).execute = [&s, i](ui::Command&) {
      if (i >= static_cast<int>(g_switch.size())) return;
      const auto& e = g_switch[static_cast<size_t>(i)];
      if (e.other) {
        Activate(e.other);
      } else if (e.extra) {
        ImGui::SetWindowFocus(("###yc_xwin" + std::to_string(e.extra)).c_str());
        if (render::GridView* v = s.ExtraView(e.extra)) {
          s.focus_sheet = v->sheet;
          s.focus_win = e.extra;
        }
      } else {
        s.focus_win = 0;
        s.focus_sheet = s.active_sheet;
        ImGui::SetWindowFocus(kGridHostWindow);
      }
    };
  }
}

// View > Focus Cell: a light band over the active cell's row and column (the cell itself stays as it is)
void DrawFocusCell(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (!s.focus_cell || sheet_id != s.focus_sheet) return;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  const auto& sel = s.FocusView().sel;
  const ImVec2 a = g.to_screen(static_cast<double>(sh->cols->Offset(sel.act_c)), static_cast<double>(sh->rows->Offset(sel.act_r)));
  const ImVec2 b = g.to_screen(static_cast<double>(sh->cols->Offset(int64_t{sel.act_c} + 1)), static_cast<double>(sh->rows->Offset(int64_t{sel.act_r} + 1)));
  const ImU32 band = IM_COL32(0x21, 0x73, 0x46, 0x26);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  dl->AddRectFilled(ImVec2(g.cells.Min.x, a.y), ImVec2(a.x, b.y), band);  // the row, left and right of the cell
  dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(g.cells.Max.x, b.y), band);
  dl->AddRectFilled(ImVec2(a.x, g.cells.Min.y), ImVec2(b.x, a.y), band);  // the column, above and below it
  dl->AddRectFilled(ImVec2(a.x, b.y), ImVec2(b.x, g.cells.Max.y), band);
  dl->PopClipRect();
}

void UpdateWindowsUi(AppState& s) {
  if (auto* c = ui::Commands().Find("FocusCell")) c->toggle = true, c->pressed = s.focus_cell;
  // the Window group's buttons follow the windows (Excel greys out what can't be done)
  bool any_hidden = s.main_hidden;
  for (const auto& w : s.extra_wins) any_hidden = any_hidden || w.hidden;
  if (auto* c = ui::Commands().Find("UnhideWindow")) c->enabled = any_hidden;
  if (auto* c = ui::Commands().Find("ViewSideBySide")) c->toggle = true, c->pressed = s.side_by_side;
  if (auto* c = ui::Commands().Find("SyncScrolling")) c->toggle = true, c->pressed = s.side_by_side && s.sync_scrolling, c->enabled = s.side_by_side;
  if (auto* c = ui::Commands().Find("ResetWindowPosition")) c->enabled = s.side_by_side;
  Partner& partner = P();
  if (partner.pid && partner.failed) partner.pid = 0;  // the other YCGrid closed
  if (s.side_by_side && s.extra_wins.empty() && !partner.pid) s.side_by_side = false;
  // Synchronous Scrolling with the other YCGrid: this workbook's scrolling is sent to it (and it sends its own here)
  if (s.side_by_side && s.sync_scrolling && partner.pid && s.extra_wins.empty()) {
    const render::GridView& v = s.wins[s.active_sheet].view;
    if (g_last_v >= 0 && (v.v.first != g_last_v || v.h.first != g_last_h)) {
      io::MV m = io::MV::Map();
      m.Set("dv", io::MV::Int(v.v.first - g_last_v));
      m.Set("dh", io::MV::Int(v.h.first - g_last_h));
      SendToPartner("layout.sync_scroll", std::move(m));
    }
    g_last_v = v.v.first;
    g_last_h = v.h.first;
  }
  // Synchronous Scrolling: the window being scrolled takes the other one along (rows and columns)
  {
    int64_t& last_v = g_last_v;
    int64_t& last_h = g_last_h;
    uint32_t& last_focus = g_last_focus;
    if (s.side_by_side && s.sync_scrolling && !s.extra_wins.empty()) {
      render::GridView& main_view = s.wins[s.active_sheet].view;
      render::GridView& other = s.extra_wins.front().view;
      render::GridView& src = s.focus_win ? other : main_view;
      render::GridView& dst = s.focus_win ? main_view : other;
      if (last_focus == s.focus_win && last_v >= 0) {
        const int64_t dv = src.v.first - last_v, dh = src.h.first - last_h;
        if (dv) dst.v.first = std::max<int64_t>(0, dst.v.first + dv);
        if (dh) dst.h.first = std::max<int64_t>(0, dst.h.first + dh);
      }
      last_v = src.v.first;
      last_h = src.h.first;
      last_focus = s.focus_win;
    } else if (!partner.pid) {
      last_v = last_h = -1;
    }
  }
  // the Switch Windows menu: this workbook's windows (refreshed twice a second) and the other YCGrid windows (looked
  // up every 2 s on a worker thread: reading the instance files and enumerating windows mustn't stall a frame)
  static std::future<std::vector<OtherWindow>> scan;
  static std::vector<OtherWindow> others;
  static double next = 0, next_scan = 0;
  const double now = ImGui::GetTime();
  if (scan.valid() && scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    others = scan.get();
    next = 0;
  }
  if (!scan.valid() && now >= next_scan) {
    scan = std::async(std::launch::async, [] { return OtherWindows(); });  // a lambda: OtherWindows has a default argument
    next_scan = now + 2.0;
  }
  if (now < next) return;
  next = now + 0.5;
  g_switch.clear();
  const std::string book = s.file_path.empty() ? std::string("Book1") : ToUtf8(fs::path(s.file_path).filename().wstring());
  g_switch.push_back({s.extra_wins.empty() ? book : book + ":1", 0, nullptr});
  for (const auto& w : s.extra_wins) g_switch.push_back({book + ":" + std::to_string(w.id), w.id, nullptr});
  // (a hidden instance lists only its own windows: Ctrl+F6 in a test went to the user's YCGrid and brought it forward)
  if (!s.headless) {
    for (const auto& o : others) g_switch.push_back({ToUtf8(o.title), 0, o.hwnd});
  }
  std::vector<std::string> menu;
  for (int i = 0; i < kSwitchSlots; ++i) {
    ui::Command* c = ui::Commands().Find("SwitchWindow" + std::to_string(i));
    if (!c) continue;
    const bool used = i < static_cast<int>(g_switch.size());
    c->label = used ? std::to_string(i + 1) + " " + g_switch[static_cast<size_t>(i)].label : "";
    c->toggle = true;
    c->pressed = used && i == 0 && s.focus_win == 0;
    if (used && g_switch[static_cast<size_t>(i)].extra) c->pressed = s.focus_win == g_switch[static_cast<size_t>(i)].extra;
    if (used) menu.push_back(c->id);
  }
  // the menu lists only the windows that exist
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "SwitchWindowsMenu") it.menu = menu;
        }
      }
    }
  }
}

}  // namespace yc::app
