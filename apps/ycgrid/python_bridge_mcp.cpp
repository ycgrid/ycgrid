// SPDX-License-Identifier: Apache-2.0
// python_bridge_mcp.cpp - tool requests for the MCP server and standalone scripts (PLAN §13, mcp/tools.json)
//
//   console.read      Immediate window console (with a cursor, only new lines)
//   python.status     host status, macros, UDFs, events, missing packages
//   python.reload     restart the host, wait for Workbook_Open and return an error summary
//   host.job          run a macro, code or a control event in the host and wait for the result (MsgBox can be answered automatically)
//   control.trigger   simulate a button Click or a combo box selection (writes the LinkedCell, then fires Change)
//   ui.screenshot     screenshot (window, grid, range)
//   ui.input          simulated keyboard and mouse (through the real UI path, fires events)
//   wb.open / wb.new  open or create a workbook
#include "trust.h"
#include "python_bridge.h"

#include "app_state.h"
#include "settings.h"
#include "engine/snapshot.h"
#include "error_log.h"
#include "model/table.h"
#include "ribbon/ribbon.h"
#include "titlebar/titlebar.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <thread>

extern int yc_imgui_assert_count;  // src/ui/yc_imgui_check.cpp

namespace yc::app {

using engine::Document;
using io::MV;

namespace {

void SetErr(std::string* code, std::string* msg, const char* c, std::string m) {
  *code = c;
  *msg = std::move(m);
}

const char* KindName(int k) {
  switch (k) {
    case 1: return "error";
    case 2: return "system";
    case 3: return "input";
    default: return "output";
  }
}

}  // namespace

MV PythonBridge::RunHostJob(MV params, int timeout_ms) {
  std::string err;
  MV r = RunJob(std::move(params), timeout_ms, false, &err);
  if (!err.empty()) {
    r = MV::Map();
    r.Set("ok", MV::Bool(false));
    r.Set("error", MV::Str(err));
  }
  return r;
}

MV PythonBridge::RunJob(MV params, int timeout_ms, bool auto_dialogs, std::string* err) {
  if (!host_ready_.load()) {
    *err = host_running_.load() ? "The Python host is still starting" : "The Python host isn't running (the folder has no main.py, or it has exited)";
    return {};
  }
  const int64_t id = next_job_++;
  auto prom = std::make_shared<std::promise<MV>>();
  auto fut = prom->get_future();
  {
    std::lock_guard<std::mutex> lk(job_mu_);
    jobs_[id] = prom;
  }
  uint64_t cursor;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    cursor = console_base_ + console_.size();
  }
  params.Set("id", MV::Int(id));
  if (auto_dialogs) ++auto_dialogs_;
  Notify("job", params);
  const auto st = fut.wait_for(std::chrono::milliseconds(std::max(100, timeout_ms)));
  if (auto_dialogs) --auto_dialogs_;
  MV out;
  if (st != std::future_status::ready) {
    std::lock_guard<std::mutex> lk(job_mu_);
    jobs_.erase(id);
    out = MV::Map();
    out.Set("ok", MV::Bool(false));
    out.Set("timeout", MV::Bool(true));
    out.Set("error", MV::Str("Timed out:" + std::to_string(timeout_ms) + " ms passed without completing (the job may still be running in the host)"));
  } else {
    out = fut.get();
  }
  // console output while it ran
  std::string text;
  {
    std::lock_guard<std::mutex> lk(state_mu_);
    const uint64_t end = console_base_ + console_.size();
    for (uint64_t i = std::max(cursor, console_base_); i < end && text.size() < 20000; ++i) {
      const auto& l = console_[static_cast<size_t>(i - console_base_)];
      text += (l.kind == 1 ? "[err] " : "") + l.text + "\n";
    }
  }
  out.Set("console", MV::Str(text));
  return out;
}

MV PythonBridge::DispatchMcp(const std::string& m, const MV& p, std::string* ec, std::string* em, bool* handled) {
  *handled = true;
  auto& eng = *s_.engine;

  if (m == "console.read") {
    const int64_t since = p.GetInt("since", -1);
    const int64_t max_lines = std::clamp<int64_t>(p.GetInt("max", 200), 1, 5000);
    const int min_kind = static_cast<int>(p.GetInt("min_kind", 0));  // 1 = errors only
    std::lock_guard<std::mutex> lk(state_mu_);
    const uint64_t end = console_base_ + console_.size();
    uint64_t start = since < 0 ? (end > static_cast<uint64_t>(max_lines) ? end - static_cast<uint64_t>(max_lines) : 0) : static_cast<uint64_t>(since);
    start = std::max(start, console_base_);
    MV lines = MV::Array();
    uint64_t i = start;
    for (; i < end && static_cast<int64_t>(lines.a.size()) < max_lines; ++i) {
      const auto& l = console_[static_cast<size_t>(i - console_base_)];
      if (min_kind == 1 && l.kind != 1) continue;
      MV x = MV::Map();
      x.Set("n", MV::Int(static_cast<int64_t>(i)));
      x.Set("kind", MV::Str(KindName(l.kind)));
      x.Set("text", MV::Str(l.text));
      lines.Push(std::move(x));
    }
    MV r = MV::Map();
    r.Set("lines", std::move(lines));
    r.Set("next_cursor", MV::Int(static_cast<int64_t>(i)));
    r.Set("truncated", MV::Bool(since >= 0 && static_cast<uint64_t>(since) < console_base_));
    return r;
  }

  if (m == "python.status") {
    MV r = MV::Map();
    r.Set("running", MV::Bool(host_running_.load()));
    r.Set("ready", MV::Bool(host_ready_.load()));
    // (none to run counts as done; so does code the Trust Center keeps disabled: nothing runs until Enable Content)
    r.Set("workbook_open_done", MV::Bool(host_opened_.load() || no_project_.load() || trust::CodeDisabled()));
    r.Set("code_disabled", MV::Bool(trust::CodeDisabled()));  // the Message Bar's "Python code has been disabled"
    r.Set("protected_view", MV::Bool(trust::InProtectedView()));  // read-only until Enable Editing
    r.Set("automation_security", MV::Int(trust::AutomationSecurity()));
    r.Set("app_dir", MV::Str(ToUtf8(app_dir_.empty() ? s_.app_dir : app_dir_)));
    r.Set("python_exe", MV::Str(ToUtf8(python_exe_)));
    std::lock_guard<std::mutex> lk(state_mu_);
    r.Set("python_version", MV::Str(python_version_));
    r.Set("last_error", MV::Str(last_error));
    MV ms = MV::Array();
    for (const auto& x : macros_) ms.Push(MV::Str(x));
    r.Set("macros", std::move(ms));
    if (const MV* u = host_info_.Get("udfs")) r.Set("udfs", *u);
    if (const MV* e = host_info_.Get("events")) r.Set("events", *e);
    if (const MV* e = host_info_.Get("modules")) r.Set("modules", *e);
    if (const MV* e = host_info_.Get("load_errors")) r.Set("load_errors", *e);
    MV miss = MV::Array();
    for (const auto& x : missing_) miss.Push(MV::Str(x));
    r.Set("missing_packages", std::move(miss));
    r.Set("missing_dismissed", MV::Bool(missing_dismissed_));
    r.Set("installing", MV::Bool(installing_));
    r.Set("install_error", MV::Str(install_error_));
    // recent errors
    MV errs = MV::Array();
    for (size_t i = console_.size(); i > 0 && errs.a.size() < 20; --i) {
      if (console_[i - 1].kind == 1) errs.Push(MV::Str(console_[i - 1].text));
    }
    std::reverse(errs.a.begin(), errs.a.end());
    r.Set("recent_errors", std::move(errs));
    return r;
  }

  if (m == "python.reload") {
    const int timeout_ms = static_cast<int>(p.GetInt("timeout_ms", 30000));
    std::wstring dir = app_dir_.empty() ? s_.app_dir : app_dir_;
    if (dir.empty()) return SetErr(ec, em, "E_APP", "The workbook has no program folder yet (save it first, or create a Python project)"), MV();
    if (trust::CodeDisabled()) {
      return SetErr(ec, em, "E_APP", "The workbook's Python code is disabled by the Trust Center: Enable Content on its Message Bar (or File > Info) runs it"), MV();
    }
    uint64_t cursor;
    {
      std::lock_guard<std::mutex> lk(state_mu_);
      cursor = console_base_ + console_.size();
    }
    host_opened_ = false;
    no_project_ = false;  // a reload runs the folder's main.py
    std::string err;
    StopHost("reload");
    const bool started = StartHost(dir, &err);
    if (!started) Log("[YCGrid]" + err, 1);
    const auto t0 = std::chrono::steady_clock::now();
    while (started && !host_opened_.load() && host_running_.load() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeout_ms)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    MV r = MV::Map();
    r.Set("started", MV::Bool(started));
    r.Set("ready", MV::Bool(host_ready_.load()));
    r.Set("workbook_open_done", MV::Bool(host_opened_.load() || no_project_.load()));  // (none to run counts as done)
    r.Set("running", MV::Bool(host_running_.load()));
    if (!started) r.Set("error", MV::Str(err));
    std::lock_guard<std::mutex> lk(state_mu_);
    MV errs = MV::Array(), out = MV::Array();
    for (uint64_t i = std::max(cursor, console_base_); i < console_base_ + console_.size(); ++i) {
      const auto& l = console_[static_cast<size_t>(i - console_base_)];
      (l.kind == 1 ? errs : out).Push(MV::Str(l.text));
    }
    r.Set("errors", std::move(errs));
    r.Set("console", std::move(out));
    MV ms = MV::Array();
    for (const auto& x : macros_) ms.Push(MV::Str(x));
    r.Set("macros", std::move(ms));
    return r;
  }

  if (m == "host.job") {
    std::string err;
    MV params = p;
    const int timeout_ms = static_cast<int>(p.GetInt("timeout_ms", 30000));
    const bool auto_dialogs = p.GetStr("dialogs", "auto") != "show";
    MV r = RunJob(std::move(params), timeout_ms, auto_dialogs, &err);
    if (!err.empty()) return SetErr(ec, em, "E_HOST", err), MV();
    return r;
  }

  if (m == "control.trigger") {
    const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    const std::string name = p.GetStr("name");
    const std::string event = p.GetStr("event", "Click");
    std::string err;
    // combo box: write the value and the LinkedCell first (like a user selection; fires per-cell OnChange and Worksheet_Change)
    std::string real_name;
    bool panel_ok = eng.Call([&](Document& d) {
      const model::Sheet* s = d.S(sheet);
      if (!s) return false;
      for (const auto& c : *s->controls) {
        std::string a = c.name, b = name;
        for (auto& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (a != b) continue;
        real_name = c.name;
        if (event == "Change" && (p.Has("value") || p.Has("list_index"))) {
          std::string value = p.GetStr("value");
          int index = static_cast<int>(p.GetInt("list_index", -1));
          std::vector<std::string> items = c.items;
          if (!c.list_fill_range.empty()) {
            uint32_t sid = 0;
            Rect rr;
            items.clear();
            if (ResolveRangeText(d.wb(), sheet, c.list_fill_range, &sid, &rr)) {
              const model::Sheet* ls = d.S(sid);
              for (uint32_t r = rr.r0; ls && r <= rr.r1 && items.size() < 10000; ++r) {
                for (uint32_t col = rr.c0; col <= rr.c1; ++col) {
                  const auto& st = d.wb().styles->Get(ls->GetStyle(r, col));
                  items.push_back(engine::DisplayTextOf(ls->GetValue(r, col), d.wb().styles->NumberFormat(st)));
                }
              }
            }
          }
          if (!p.Has("value") && index >= 0 && static_cast<size_t>(index) < items.size()) value = items[static_cast<size_t>(index)];
          if (index < 0) {
            for (size_t i = 0; i < items.size(); ++i) {
              if (items[i] == value) index = static_cast<int>(i);
            }
          }
          d.Begin("Change" + c.name);
          try {
            ApplyControlValue(d, sheet, c.id, value, index);
          } catch (...) {
            d.Commit();
            throw;
          }
          d.Commit();
        } else if (event == "Click" && (c.kind == model::ControlKind::CheckBox || c.kind == model::ControlKind::ToggleButton ||
                                        c.kind == model::ControlKind::OptionButton)) {
          // a click on a check box / toggle button flips it; on an option button it selects it
          const bool on = c.kind == model::ControlKind::OptionButton ? true : !model::ControlIsOn(c);
          d.Begin("Change" + c.name);
          try {
            ApplyControlValue(d, sheet, c.id, on ? "True" : "False", -1);
          } catch (...) {
            d.Commit();
            throw;
          }
          d.Commit();
        }
        return true;
      }
      return false;
    });
    if (!panel_ok) return SetErr(ec, em, "E_NOT_FOUND", "Control not found:" + name), MV();
    MV jp = MV::Map();
    jp.Set("kind", MV::Str("control"));
    jp.Set("sheet", MV::Int(sheet));
    jp.Set("name", MV::Str(real_name));
    jp.Set("event", MV::Str(event));
    eng.Sync();  // let the LinkedCell change events go out first
    MV r = RunJob(std::move(jp), static_cast<int>(p.GetInt("timeout_ms", 10000)), p.GetStr("dialogs", "auto") != "show", &err);
    if (!err.empty()) return SetErr(ec, em, "E_HOST", err), MV();
    return r;
  }

  if (m == "host.eval") {
    // run code in the Python host like the Immediate window; starts the host (in a scratch folder) when none runs.
    // shown=false runs it as a dialog's code (errors go to the Error Log, the Immediate window may be closed)
    const std::string code = p.GetStr("code");
    const bool shown = p.GetBool("shown", true);
    OnUi([code, shown](AppState& s) {
      RunPythonInHost(s, code, shown);
      return MV();
    });
    return MV::Bool(true);
  }
  if (m == "ui.items") {  // [YC] tests: the labelled controls drawn in the last frame (nil: recording just started)
    const std::string win = p.GetStr("window");
    return OnUi([win](AppState&) {
      if (!ui::RecordingUiItems()) {
        ui::RecordUiItems(true);
        return MV();
      }
      MV a = MV::Array();
      for (const auto& it : ui::UiItems(!win.empty())) {  // in one window: its unlabelled controls too
        if (!win.empty() && it.window.find(win) == std::string::npos) continue;
        MV x = MV::Map();
        x.Set("label", MV::Str(it.label));
        x.Set("id", MV::Str(it.id));
        x.Set("window", MV::Str(it.window));
        x.Set("disabled", MV::Bool(it.disabled));
        if (it.checked >= 0) x.Set("checked", it.checked == 2 ? MV::Str("mixed") : MV::Bool(it.checked == 1));
        MV r = MV::Array();
        for (float v : {it.rect.Min.x, it.rect.Min.y, it.rect.Max.x, it.rect.Max.y}) r.Push(MV::Double(v));
        x.Set("rect", std::move(r));
        a.Push(std::move(x));
      }
      return a;
    });
  }
  if (m == "ui.ribbon_items") {  // [YC] tests: the ribbon buttons, menu entries, gallery tiles shown now, to click them
    return OnUi([](AppState&) {
      MV a = MV::Array();
      for (const auto& it : ui::ribbon::VisibleItems()) {
        MV x = MV::Map();
        x.Set("cmd", MV::Str(it.cmd));
        x.Set("where", MV::Str(it.where));
        x.Set("label", MV::Str(it.label));
        MV r = MV::Array();
        for (float v : {it.rect.Min.x, it.rect.Min.y, it.rect.Max.x, it.rect.Max.y}) r.Push(MV::Double(v));
        x.Set("rect", std::move(r));
        a.Push(std::move(x));
      }
      return a;
    });
  }
  if (m == "ui.answer_file_dialogs") {  // [YC] tests: what the next file dialogs of a hidden instance choose ("" = Cancel)
    std::vector<std::wstring> paths;
    if (const MV* a = p.Get("paths")) {
      for (const MV& x : a->a) paths.push_back(FromUtf8(x.s));
    }
    QueueFileDialogAnswers(paths);
    return MV::Int(static_cast<int64_t>(paths.size()));
  }
  if (m == "ui.cursor_design") {  // [YC] tests: an Excel cursor's 32 x 32 design by name ("cell", "fill"...) and hotspot
    const std::string name = p.GetStr("name");
    for (int c = render::kCursorCell; c <= render::kCursorRowUnhide; ++c) {
      if (name != render::CursorName(c)) continue;
      int hx = 0, hy = 0;
      MV rows = MV::Array();
      for (const std::string& r : render::CursorDesign(c, &hx, &hy)) rows.Push(MV::Str(r));
      MV o = MV::Map();
      o.Set("rows", std::move(rows));
      o.Set("hot_x", MV::Int(hx));
      o.Set("hot_y", MV::Int(hy));
      return o;
    }
    return MV();
  }
  if (m == "ui.ping") {  // [YC] the UI thread answers, without waiting for the engine: a frozen screen check
    return OnUi([](AppState&) { return MV::Bool(true); }, false);
  }
  if (m == "ui.state") {
    // everything a test or an agent checks without a screenshot: the workbook, the selection, dialogs, safety, data,
    // figures, replay (headless verification)
    eng.Sync();
    const TapeStatus tape = GetTapeStatus();
    MV r = OnUi([](AppState& s) {
      MV o = MV::Map();
      o.Set("pid", MV::Int(static_cast<int64_t>(::GetCurrentProcessId())));
      o.Set("visible", MV::Bool(::IsWindowVisible(s.hwnd) != 0));
      o.Set("file", MV::Str(ToUtf8(s.file_path)));
      o.Set("title", MV::Str(ui::titlebar::St().title));  // the title bar: "Book1 [Group] - YCGrid"
      {
        // the title bar's clickable areas Windows is told about (WM_NCHITTEST: the rest of the bar drags the window),
        // in the coordinates of ui.items: a button missing here can't be clicked with a real mouse
        MV hits = MV::Array();
        const ImVec2 org = ImGui::GetMainViewport()->Pos;
        for (const RECT& rc : ui::titlebar::St().interactive) {
          MV a = MV::Array();
          for (double v : {rc.left + org.x, rc.top + org.y, rc.right + org.x, rc.bottom + org.y}) a.Push(MV::Double(v));
          hits.Push(std::move(a));
        }
        o.Set("titlebar_clickable", std::move(hits));
      }
      o.Set("dialog", MV::Int(static_cast<int64_t>(s.dialog)));
      o.Set("dialog_text", MV::Str(DialogText(s)));  // the open message or question: its title, then its text
      {
        MV el = MV::Map();  // the status bar's Error Log entry: what it counts as new
        el.Set("errors", MV::Int(ErrorsUnseen()));
        el.Set("messages", MV::Int(MessagesUnseen()));
        // the red line beside it while it shows (a refusal on a protected sheet), else ""
        el.Set("refusal", MV::Str(ImGui::GetTime() < s.refusal_until ? s.refusal : std::string()));
        o.Set("error_log", std::move(el));
      }
      o.Set("focus_window", MV::Int(s.focus_win));
      o.Set("chart_tip", MV::Str(s.chart_tip));  // the tip over a chart the last frame showed ("" none)
      MV shrink = MV::Array();
      for (const auto& n : s.shrinking_windows) shrink.Push(MV::Str(n));
      o.Set("shrinking_windows", std::move(shrink));
      MV clipped = MV::Array();
      for (const auto& n : s.clipped_windows) clipped.Push(MV::Str(n));
      o.Set("clipped_windows", std::move(clipped));  // dialogs whose contents are cut off on the right (should stay empty)
      o.Set("vscode_request", MV::Str(LastVsCodeRequest()));  // a hidden instance opens no VS Code: what it would have opened
      // nor anything else on the desktop: the last link, file or folder it would have opened, and how many times
      o.Set("desktop_open", MV::Str(ToUtf8(LastDesktopOpen())));
      o.Set("desktop_opens", MV::Int(DesktopOpens()));
      MV wins = MV::Array();  // the top-level windows of the last frame: name, [x, y, width, height], fits its contents, popup
      for (const auto& w : s.window_rects) {
        MV e = MV::Map();
        e.Set("name", MV::Str(w.name));
        MV r = MV::Array();
        for (float v : {w.x, w.y, w.w, w.h}) r.Push(MV::Double(v));
        e.Set("rect", std::move(r));
        e.Set("auto_size", MV::Bool(w.auto_size));
        e.Set("popup", MV::Bool(w.popup));
        wins.Push(std::move(e));
      }
      o.Set("windows", std::move(wins));  // windows that narrowed by themselves (should stay empty)  // the window with the keyboard: 0 the main one, else New Window's number
      o.Set("status", MV::Str(s.status));
      o.Set("imgui_asserts", MV::Int(yc_imgui_assert_count));  // ImGui's checks that failed (a debug build stops on one)
      o.Set("status_stats", MV::Str(s.status_stats));
      o.Set("circular_status", MV::Str(CircularStatusText(s)));  // the status bar's "Circular References: B2"
      o.Set("copy_mode", MV::Bool(s.marquee));  // copied cells outlined (Excel's CutCopyMode): Enter or Paste pastes
      o.Set("interactive", MV::Bool(s.interactive));
      o.Set("trust", trust::StateMV());  // the Trust Center: code none / enabled / disabled, the Message Bar shown
      o.Set("operator_mode", MV::Bool(s.operator_mode));
      o.Set("cursor", MV::Str(render::CursorName(render::LastFrameCursor())));
      o.Set("validation_circles", MV::Int(ValidationCircleCount(s.focus_sheet)));  // Circle Invalid Data on the sheet shown  // the mouse cursor's shape ("cell", "fill", "column_unhide"...)
      {  // the ribbon: collapsed (tabs only), shown for a moment (peek), the selected tab, any popup open over it
        MV rb = MV::Map();
        const auto& st = ui::ribbon::St();
        rb.Set("collapsed", MV::Bool(st.collapsed));
        rb.Set("peek", MV::Bool(st.peek));
        rb.Set("tab", MV::Int(st.selected_tab));
        // its label as shown (Table Design after Ctrl+T): tests read which tab is up without counting the hidden ones
        if (const auto& tl = ui::ribbon::Tabs(); st.selected_tab >= 0 && st.selected_tab < static_cast<int>(tl.size()))
          rb.Set("tab_label", MV::Str(tl[static_cast<size_t>(st.selected_tab)].label));
        rb.Set("backstage", MV::Bool(st.backstage));
        rb.Set("keytips", MV::Int(st.keytip_level));
        if (!st.legacy_keys.empty()) rb.Set("legacy_keys", MV::Str(st.legacy_keys));
        rb.Set("popup_open", MV::Bool(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)));
        auto rect = [](const ImRect& r) {  // the centre, in the window's pixels (what ui.input's clicks take)
          MV x = MV::Array();
          x.Push(MV::Double(r.GetCenter().x));
          x.Push(MV::Double(r.GetCenter().y));
          return x;
        };
        MV tabs = MV::Map();
        for (const auto& [label, r] : st.tab_rects) tabs.Set(label, rect(r));
        rb.Set("tabs", std::move(tabs));
        rb.Set("arrow", rect(st.arrow_rect));
        o.Set("ribbon", std::move(rb));
      }
      MV alarms = MV::Array();
      for (const auto& a : SafetyAlarms(s)) alarms.Push(MV::Str(a));
      o.Set("alarms", std::move(alarms));
      if (s.snap) {
        const auto& wb = s.snap->wb;
        o.Set("dirty", MV::Bool(wb.dirty));
        // the undo / redo lists: how many steps and the label of the next one (a test counts the steps an action made)
        MV undo = MV::Map();
        undo.Set("depth", MV::Int(s.snap->undo_depth));
        undo.Set("made", MV::Int(static_cast<int64_t>(s.snap->steps_made)));  // steps ever made (the depth stays at the limit)
        undo.Set("label", MV::Str(s.snap->undo_label));
        o.Set("undo", std::move(undo));
        MV redo = MV::Map();
        redo.Set("depth", MV::Int(s.snap->redo_depth));
        redo.Set("label", MV::Str(s.snap->redo_label));
        o.Set("redo", std::move(redo));
        MV sheets = MV::Array();
        for (const auto& sh : wb.sheets) {
          MV m = MV::Map();
          m.Set("id", MV::Int(sh->id));
          m.Set("name", MV::Str(sh->name));
          m.Set("visible", MV::Bool(sh->visible));
          m.Set("used", MV::Str(sh->has_used ? core::CellA1(sh->used.r0, sh->used.c0) + ":" + core::CellA1(sh->used.r1, sh->used.c1) : ""));
          m.Set("tables", MV::Int(static_cast<int64_t>(sh->tables->size())));
          m.Set("charts", MV::Int(static_cast<int64_t>(sh->charts->size())));
          sheets.Push(std::move(m));
        }
        o.Set("sheets", std::move(sheets));
        if (const model::Sheet* fs = s.FocusSheet()) {
          const auto& sel = s.FocusView().sel;
          o.Set("active_sheet", MV::Str(fs->name));
          o.Set("active_cell", MV::Str(core::CellA1(sel.act_r, sel.act_c)));
          const Rect a = SelRect(s);
          o.Set("selection", MV::Str(a.IsCell() ? core::CellA1(a.r0, a.c0) : core::CellA1(a.r0, a.c0) + ":" + core::CellA1(a.r1, a.c1)));
          const core::Value& v = fs->GetValue(sel.act_r, sel.act_c);
          o.Set("active_value", v.IsNumeric() ? MV::Double(v.AsDouble()) : v.IsBool() ? MV::Bool(v.boolean())
                                : v.IsEmpty() ? MV() : MV::Str(engine::DisplayTextOf(v, "General")));
          o.Set("active_text", MV::Str(engine::InputTextOf(wb, fs->id, sel.act_r, sel.act_c)));
        }
        if (s.edit.active) {
          // the cell being typed into and its text so far (lets tests follow keyboard pointing, F4, autocomplete)
          MV e = MV::Map();
          e.Set("cell", MV::Str(core::CellA1(s.edit.r, s.edit.c)));
          e.Set("text", MV::Str(s.edit.text));
          e.Set("caret", MV::Int(s.edit.caret));
          e.Set("formula_bar", MV::Bool(s.edit.in_formula_bar));
          e.Set("mode", MV::Str(s.edit.enter_mode ? "enter" : "edit"));
          // the reference being pointed at (its span in the text; -1: none), as a click or an arrow key replaces it
          e.Set("ref_begin", MV::Int(s.edit.ref_begin));
          e.Set("ref_end", MV::Int(s.edit.ref_end));
          o.Set("edit", std::move(e));
        }
        MV bound = MV::Array();
        for (const auto& b : *wb.bound_tables) {
          MV m = MV::Map();
          m.Set("name", MV::Str(b.name));
          uint32_t sid = 0;
          const model::Table* t = model::FindTable(wb, b.name, &sid);
          m.Set("rows", MV::Int(t && t->HasData() ? static_cast<int64_t>(t->Data().Rows()) : 0));
          m.Set("record", MV::Bool(b.record));
          m.Set("stale_after", MV::Double(b.stale_after));
          bound.Push(std::move(m));
        }
        o.Set("bound_tables", std::move(bound));
      }
      o.Set("figures", FiguresState());
      return o;
    });
    MV rp = MV::Map();
    static const char* modes[] = {"live", "replay", "paused", "ended"};
    rp.Set("mode", MV::Str(modes[std::clamp(tape.mode, 0, 3)]));
    rp.Set("recording", MV::Bool(tape.recording));
    r.Set("replay", std::move(rp));
    MV host = MV::Map();
    host.Set("running", MV::Bool(HostRunning()));
    host.Set("ready", MV::Bool(HostReady()));
    r.Set("python", std::move(host));
    return r;
  }

  if (m == "ui.screenshot") {
    eng.Sync();  // show what Python just wrote, even while live changes are throttled
    auto prom = std::make_shared<std::promise<MV>>();
    auto fut = prom->get_future();
    AppState::ShotRequest req;
    req.path = FromUtf8(p.GetStr("path"));
    req.target = p.GetStr("target", "grid");
    req.address = p.GetStr("address");
    req.sheet = static_cast<uint32_t>(p.GetInt("sheet", 0));
    req.max_width = static_cast<int>(p.GetInt("max_width", 1400));
    req.headers = p.GetBool("include_headers", true);
    req.to_clipboard = p.GetBool("to_clipboard", false);  // Range.CopyPicture
    req.done = [prom](bool ok, int w, int h, const std::string& info) {
      MV r = MV::Map();
      r.Set("ok", MV::Bool(ok));
      r.Set("width", MV::Int(w));
      r.Set("height", MV::Int(h));
      r.Set("info", MV::Str(info));
      prom->set_value(r);
    };
    OnUi([req](AppState& s) {
      s.shots.push_back(req);
      return MV();
    });
    if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) return SetErr(ec, em, "E_TIMEOUT", "Screenshot timed out (the window may be minimized)"), MV();
    MV r = fut.get();
    if (!r.GetBool("ok")) return SetErr(ec, em, "E_APP", "Screenshot failed:" + r.GetStr("info")), MV();
    return r;
  }

  if (m == "spell.check") {
    const std::string word = p.GetStr("word"), lang = p.GetStr("lang", "en-US");
    return OnUi([&](AppState&) {
      std::vector<std::string> sugg;
      const bool ok = SpellCheckWord(lang, word, &sugg);
      MV o = MV::Map();
      o.Set("ok", MV::Bool(ok));
      MV a = MV::Array();
      for (const auto& x : sugg) a.Push(MV::Str(x));
      o.Set("suggestions", std::move(a));
      return o;
    });
  }
  if (m == "ui.cell_rect") {
    const std::string addr = p.GetStr("address");
    std::string err;
    MV r = OnUi([&, addr](AppState&) {
      float q[4];
      if (!RangeWindowRect(addr, q)) {
        err = "Not a range of the active sheet: " + addr;
        return MV::Nil();
      }
      MV a = MV::Array();
      for (float v : q) a.Push(MV::Double(v));
      return a;
    });
    if (!err.empty()) SetErr(ec, em, "E_ARG", err);
    return r;
  }
  if (m == "ui.input") {
    const std::string script = p.GetStr("script");
    const int timeout_ms = static_cast<int>(p.GetInt("timeout_ms", 15000));
    const bool auto_dialogs = p.GetStr("dialogs", "auto") != "show";
    struct AutoDialogs {  // restored even when OnUi throws (the UI is not responding, YCGrid is closing)
      std::atomic<int>& n;
      bool on;
      ~AutoDialogs() {
        if (on) --n;
      }
    } auto_guard{auto_dialogs_, auto_dialogs};
    if (auto_dialogs) ++auto_dialogs_;
    const MV refused = OnUi([script](AppState&) {
      const std::string e = QueueUiScript(script);
      return e.empty() ? MV() : MV::Str(e);
    });
    if (refused.t == MV::T::Str) {
      *ec = "E_ARG";
      *em = refused.s;
      return {};
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool busy = true;
    while (busy && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(timeout_ms)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      busy = OnUi([](AppState&) { return MV::Bool(UiScriptBusy()); }).b;
    }
    eng.Sync();
    MV r = MV::Map();
    r.Set("done", MV::Bool(!busy));
    return r;
  }

  if (m == "wb.open" || m == "wb.new") {
    const std::string path = p.GetStr("path");
    const bool discard = p.GetBool("discard_changes");
    const std::string password = p.GetStr("password");
    std::string err;
    // the UI's snapshot can still be from before the latest writes: check and clear "unsaved" on the current one,
    // otherwise NewWorkbook asks "Do you want to save changes?" with a message box and the request hangs
    eng.Sync();
    MV r = OnUi([&, path, discard](AppState& s) {
      s.snap = s.engine->Current();
      if (s.snap && s.snap->wb.dirty && !discard) {
        err = "The workbook has unsaved changes; pass discard_changes=true to discard them";
        return MV();
      }
      s.engine->Call([](Document& d) { d.wb_mut().dirty = false; });
      s.engine->Sync();
      s.snap = s.engine->Current();
      std::string why;
      const bool ok = m == "wb.new" ? NewWorkbook(s) : OpenWorkbook(s, FromUtf8(path), password, false, &why);
      if (!ok) err = m == "wb.new" ? "Couldn't create a new workbook" : "Couldn't open " + path + (why.empty() ? std::string() : ": " + why);
      return MV::Bool(ok);
    });
    if (!err.empty()) return SetErr(ec, em, "E_APP", err), MV();
    return r;
  }

  *handled = false;
  return MV();
}

}  // namespace yc::app
