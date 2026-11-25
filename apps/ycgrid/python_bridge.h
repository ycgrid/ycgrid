// SPDX-License-Identifier: Apache-2.0
// python_bridge.h - YCP server: connects the Python host and standalone scripts (PLAN §13)
//
// Threads:
//   - host reader thread: reads Python -> YCGrid requests, handles them and writes replies (model operations via Engine::Call,
//     view operations via the UI command queue, applied on the next frame)
//   - controller listener thread: one duplex pipe per standalone script, request / reply
//   - engine thread: OnChanges / RequestUdf / Subscribe send notifications to the host (writes are locked, never block)
#pragma once

#include "engine/bind.h"
#include "engine/document.h"
#include "engine/tape.h"
#include "io/msgvalue.h"
#include "topic_subs.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace yc::app {

struct AppState;

struct ConsoleLine {
  std::string text;
  int kind = 0;  // 0 output, 1 error, 2 system, 3 input
};

// custom_ribbon.cpp (RibbonX): the loaded customUI.xml and its controls' states
io::MV RibbonInfo();
bool ActivateRibbonTab(const std::string& id);  // a custom tab's id or a built-in idMso

class PythonBridge final : public engine::ExternalProvider {
 public:
  void WriteInstanceFile();  // %LOCALAPPDATA%\ycgrid\instances\<pid>.json (pid, pipe, folder, file)
  // the file this window was started with couldn't be opened: Workbooks.Open in the window that started it raises the
  // error instead of taking this blank workbook for the file
  void NoteOpenFailed(const std::wstring& path, const std::string& error);
  // while the file this window was started with is still being read, the instance file names no file yet
  void SetStartupOpen(bool pending) { startup_open_ = pending; }
  explicit PythonBridge(AppState& s);
  ~PythonBridge() override;

  // controller pipe and instance registration (%LOCALAPPDATA%\ycgrid\instances\<pid>.json)
  void StartListener();
  // start the Python host (app_dir is the project folder); on false, error explains why
  bool StartHost(const std::wstring& app_dir, std::string* error);
  void StopHost(const char* reason = "close");  // reason: "close" (the workbook closes), "restart", "reload"
  // data feed processes of [[python.feeds]] in ycgrid.toml (python_feeds.cpp); started and stopped with the host
  std::vector<std::string> FeedNames() const;
  void RestartHost();
  bool HostRunning() const { return host_running_.load(); }
  // the open workbook has no Python project (no main.py): there is no Workbook_Open to wait for
  void SetNoProject(bool v) { no_project_ = v; }
  bool NoProject() const { return no_project_.load(); }  // the workbook's folder holds no Python project
  void StartHostForFormulas();  // =PY needs the host: start it on the UI thread (any thread)
  bool HostReady() const { return host_ready_.load(); }
  std::wstring HostAppDir() const { return app_dir_; }

  // UI thread: apply view operations sent by Python at the start of every frame
  void ProcessUiQueue();
  // the main loop has ended: requests waiting for the UI thread (and new ones) fail at once instead of timing out, so
  // closing YCGrid while Python is busy drawing doesn't wait 30 s per waiting thread
  void CloseUi();

  // UI -> host
  // Excel's events (events_ui.cpp fires them): name is the sheet module's ("Worksheet_Calculate"; the host also runs
  // main.py's Workbook_SheetCalculate) or a workbook event ("Workbook_AfterSave"); extra: the event's own arguments
  void SendEvent(const std::string& name, uint32_t sheet = 0, const std::vector<core::Rect>* areas = nullptr, const io::MV* extra = nullptr);
  // the project has a handler for the event (either the Worksheet_ or the Workbook_Sheet one); sheet: for that sheet
  // (its module's handler, or main.py's Workbook_Sheet one); 0 = any sheet
  bool WantsEvent(const std::string& name, uint32_t sheet = 0);
  // a Before* event: false when nobody handles it (act now); else done(cancelled) runs on the UI thread once the host
  // answered, or as not cancelled after timeout_ms (a hung handler can't stop YCGrid), or when the host stops
  bool AskEvent(const std::string& name, uint32_t sheet, const std::vector<core::Rect>* areas, io::MV extra,
                std::function<void(AppState&, bool)> done, int timeout_ms = 2000);
  void TickAsks();  // UI thread, every frame: Before* events past their time go on
  bool AsksPending();
  // engine thread: Workbook_NewSheet / Worksheet_Calculate for a snapshot's added and calculated sheets
  void OnSheetEvents(const std::vector<uint32_t>& added, const std::vector<uint32_t>& calculated);
  // Workbook_BeforeClose already ran for this close (the window asked it): the host's shutdown doesn't run it again
  void SetBeforeCloseDone(bool v) { before_close_done_ = v; }
  void RunMacro(const std::string& name);
  // liveness of the host (safety): a ping it answers from its event loop; a busy or hung host doesn't answer
  void PingHost() { Notify("ping", io::MV::Map()); }
  void NotifyHost(const std::string& method, const io::MV& params) { Notify(method, params); }
  double LastPong() const { return last_pong_.load(); }  // steady-clock seconds; 0 = never
  bool HostCrashed() const { return host_crashed_.load(); }  // it exited without being asked to (until the next start)
  void InstallPackages(bool always);
  // shown=false: code run for a dialog or a script (host.eval with shown=false): its errors go to the Error Log, not only the Immediate window
  void ConsoleEval(const std::string& code, bool shown = true);
  void ControlEvent(uint32_t sheet, const std::string& name, const std::string& event, const std::string& on_action, bool panel = false);
  void RibbonNotify(const io::MV& params) { Notify("ribbon", params); }  // RibbonX callbacks (custom_ribbon.cpp)
  void LogSystem(std::string text) { Log(std::move(text), 2); }
  void LogError(std::string text) { Log(std::move(text), 1); }

  // status shown by the UI (read on the UI thread; protected by a mutex)
  std::vector<ConsoleLine> ConsoleSnapshot();
  void ClearConsole();
  std::vector<std::string> Macros();
  std::vector<std::string> UdfNames();  // @udf functions registered by the host (formula autocomplete)
  std::vector<std::string> MissingPackages();
  // what the package bar shows: the missing packages, whether they are being installed, and why the last install
  // failed ("" when it didn't); packages empty: no bar
  struct PackageBarState {
    std::vector<std::string> packages;
    bool installing = false;
    std::string error;
  };
  PackageBarState PackageBar();
  void DismissMissing();
  std::string StatusText();
  std::wstring python_exe() const { return python_exe_; }
  std::string last_error;

  // ---- recordings of bound-table data (replay.cpp): record the frames that arrive, replay them into the same tables.
  // While a replay runs, live frames for the tables on the tape are dropped, so a screen switches between live and
  // recorded data without code changes; going back to live restores the tables as they were.
  struct TapeStatus {
    bool recording = false;
    std::string rec_path;
    double rec_seconds = 0;
    uint64_t rec_frames = 0, rec_bytes = 0;
    int mode = 0;  // 0 live, 1 replaying, 2 paused, 3 at the end
    std::string path;
    double pos_s = 0, length_s = 0, speed = 1;
    bool loop = false;
    int64_t start_unix_ms = 0;  // when the tape was recorded
    uint64_t frame = 0, frames = 0, dropped = 0;
    std::vector<std::string> tables;
    struct Mark {
      double t = 0;  // seconds
      std::string kind, text;
    };
    std::vector<Mark> marks;  // the events on the tape's timeline
    bool pinned = false;      // a moment was pinned for Compare
    double pin_s = 0;
  };
  // the time machine: figure requests and markers on the tape, and comparing two moments
  void TapePlot(const std::string& method, const io::MV& params, const std::shared_ptr<engine::RawFrame>& data);
  void TapeMark(const std::string& kind, const std::string& text);
  bool PlotOnTape(int64_t fig);  // a replay owns this figure (live requests for it are dropped)
  bool PinTables();              // remember the recorded tables as they are now (Compare)
  io::MV CompareWithPin(size_t limit);  // [{table, key, column, then, now, change}]
  // tables: the bound tables to record ("" or empty = all; "*" = everything: the app's tables, the figures, markers);
  // figures: the figure requests that rebuild the open figures (FigureSnapshot(), taken on the UI thread)
  using PlotRecords = std::vector<std::tuple<std::string, io::MV, std::shared_ptr<engine::RawFrame>>>;
  bool StartRecording(const std::wstring& path, const std::vector<std::string>& tables, std::string* error, const PlotRecords* figures = nullptr);
  // returns the frames written, 0 when nothing was (the file is removed then), -1 when nothing was being recorded
  int64_t StopRecording();
  bool StartReplay(const std::wstring& path, double speed, double from_s, bool loop, std::string* error);
  void PauseReplay(bool pause);
  void SeekReplay(double seconds);
  void SetReplaySpeed(double speed);  // 0 = as fast as possible
  void SetReplayLoop(bool loop);
  void StopReplay() { EndReplay(true); }  // back to live
  TapeStatus GetTapeStatus();

  // runs code in the host's Immediate namespace and waits (any thread but the UI's): result,
  // error, traceback, console, ms
  io::MV RunHostJob(io::MV params, int timeout_ms);  // any host job (e.g. kind "complete"); errors come back as ok = false

  // engine::ExternalProvider (engine thread)
  void RequestUdf(const std::string& key, const std::string& name, const std::vector<std::shared_ptr<formula::Array>>& args, uint32_t sheet,
                  uint32_t row, uint32_t col) override;
  void Subscribe(const std::string& topic, const std::vector<core::Value>& args) override;
  // change notifications from the engine (engine thread)
  void OnChanges(const std::vector<engine::CellChange>& changes, uint32_t focus_sheet);

  // find the portable Python (PLAN §13 (d)): ycgrid.toml -> app_dir\runtime -> exe folder -> build\runtime during development
  static std::wstring FindPython(const std::wstring& app_dir);
  static std::wstring FindYcPackage();

 private:
  // a shared-memory ring of column frames (python_bridge_data.cpp)
  struct DataChannel {
    HANDLE mapping = nullptr;
    uint8_t* view = nullptr;
    uint64_t size = 0;
    std::string name;
    ~DataChannel();
  };
  struct Conn {
    HANDLE read = INVALID_HANDLE_VALUE;
    HANDLE write = INVALID_HANDLE_VALUE;
    std::mutex wmu;
    bool host = false;
    int64_t id = 0;      // the connection's number (figures remember who made them)
    unsigned long pid = 0;  // the client's process (controllers)
    std::mutex chan_mu;
    std::vector<std::shared_ptr<DataChannel>> channels;  // closed with the connection
    std::shared_ptr<DataChannel> out;  // table.read: the last table read, for the client to copy (grows as needed)
  };

  void HostReaderLoop(std::shared_ptr<Conn> c, HANDLE process);
  void ConnClosed(const Conn& c);  // its figures close (or show "(stopped)")
  std::atomic<int64_t> next_conn_{1};
  void ControllerLoop();
  void ServeController(std::shared_ptr<Conn> c);
  bool ReadFrame(Conn& c, std::string* out);
  bool WriteFrame(Conn& c, const std::string& data);
  void Reply(Conn& c, int64_t id, const io::MV* err_code, const std::string& err_msg, const io::MV& result);
  void Notify(const std::string& method, const io::MV& params);
  // request dispatch: returns the result; sets err_code / err_msg on failure
  io::MV Dispatch(Conn& c, const std::string& method, const io::MV& p, std::string* err_code, std::string* err_msg);
  // controls, charts, panels, containers, layout (python_bridge_objects.cpp)
  io::MV DispatchObjects(const std::string& method, const io::MV& p, std::string* err_code, std::string* err_msg, bool* handled);
  // tool requests for MCP and standalone scripts: console, host status and reload, jobs in the host, screenshots, simulated input, files (python_bridge_mcp.cpp)
  // the shared-memory data channel and bound tables (python_bridge_data.cpp)
  io::MV DispatchData(Conn& c, const std::string& method, const io::MV& p, std::string* err_code, std::string* err_msg, bool* handled);
  std::map<std::string, std::string> bind_errors_;
  // frames waiting for the engine: a frame that only repeats the previous one's keys and columns with newer values
  // replaces it, so a fast sender never builds a backlog (like the topic conflation table)
  struct BindInbox {
    std::mutex mu;
    std::deque<std::shared_ptr<engine::ColumnFrame>> frames;
    bool posted = false;
    uint64_t merged = 0;
    std::atomic<uint64_t> drains{0}, breaks{0}, applied{0};
    uint64_t cells = 0;  // cells waiting; above kBindBacklog a sender waits (its ring fills, so Python slows down)
    std::condition_variable cv;
  };
  BindInbox bind_inbox_;
  std::atomic<double> last_pong_{0};
  std::atomic<bool> host_crashed_{false};
  // queues a decoded frame for the engine (bind.frame and replays); wait = backpressure while the engine is behind
  void EnqueueFrame(std::shared_ptr<engine::ColumnFrame> frame, bool wait);
  // recording and replay state (replay.cpp)
  struct Tape {
    std::mutex mu;
    // recording
    engine::TapeWriter writer;
    std::string rec_path;
    int64_t rec_start_ns = 0;
    std::vector<std::string> rec_tables;  // lowercase; empty = all
    // replay
    std::thread th;
    std::condition_variable cv;
    engine::TapeReader reader;
    std::string path;
    std::vector<std::string> tables;  // lowercase names on the tape (live frames for them are dropped)
    std::unordered_set<std::string> topics;  // topics on the tape (live pushes for them are dropped)
    bool rec_topics = false;  // topic pushes are recorded too
    std::vector<std::shared_ptr<engine::ColumnFrame>> live_state;  // the tables before the replay
    std::atomic<bool> replaying{false};
    bool quit = false, paused = false, loop = false, ended = false;
    double speed = 1;
    int64_t seek_ns = -1;  // a seek waiting for the replay thread
    size_t next = 0;       // the next frame
    int64_t pos_ns = 0;
    uint64_t control = 0;  // changes when a control changes (wakes the replay thread)
    std::atomic<uint64_t> dropped{0};
    std::unordered_set<int64_t> plot_figs;  // figures on the tape
    bool rec_everything = false;            // app tables and figures too (the time machine)
    std::vector<std::shared_ptr<engine::ColumnFrame>> pin;  // Compare: the tables at the pinned moment
    double pin_s = 0;
    bool pinned = false;
  };
  Tape tape_;
  void ReplayLoop();
  void EndReplay(bool restore);
  // bind.frame: records the frame when recording; true when a replay owns its table (the frame is dropped)
  bool TapeFrame(const engine::ColumnFrame& f, const uint8_t* raw, size_t len);
  // stream.push: records the topic updates when recording; removes those a replay owns
  void TapeTopics(std::vector<std::pair<std::string, core::Value>>* items);
  // Application.Topics.Subscribe: the updates (from Push or a replay) for the host's subscriptions
  void FeedTopicSubs(const std::vector<std::pair<std::string, core::Value>>& items, const std::vector<int64_t>* times);
  void PostBindDrain();  // last error of asynchronous frames, by lowercase table name (state_mu_)
  io::MV DispatchMcp(const std::string& method, const io::MV& p, std::string* err_code, std::string* err_msg, bool* handled);
  io::MV RunJob(io::MV params, int timeout_ms, bool auto_dialogs, std::string* err);
  // run on the UI thread and wait for the result
  // sync: publish what is queued for the engine first (the UI sees this connection's writes); false for ui.ping
  io::MV OnUi(std::function<io::MV(AppState&)> fn, bool sync = true);
  void Log(std::string text, int kind, bool report = true);  // report: an error (kind 1) also goes to the Error Log
  struct Feed {
    std::string name;
    std::wstring script;
    bool restart = true;
    std::mutex mu;
    HANDLE process = nullptr;  // protected by mu
    HANDLE stop_event = nullptr;
    std::thread runner;
    std::atomic<bool> done{false};  // the runner has returned
  };
  std::mutex feeds_mu_;  // StartFeeds / StopFeeds (the UI thread, host restarts)
  void StartFeeds();
  void StopFeeds();
  void RunFeed(Feed* f, const std::wstring& ycdir, const std::wstring& app_dir);
  std::vector<std::unique_ptr<Feed>> feeds_;

  AppState& s_;
  std::wstring app_dir_;
  std::wstring python_exe_;
  std::shared_ptr<Conn> host_;  // under host_mu_: the engine and UI threads Notify while a controller restarts the host
  std::mutex host_mu_;
  std::shared_ptr<Conn> Host() {
    std::lock_guard<std::mutex> lk(host_mu_);
    return host_;
  }
  HANDLE host_process_ = nullptr;
  HANDLE job_ = nullptr;
  std::thread host_thread_;
  std::atomic<bool> host_running_{false}, host_ready_{false}, stopping_{false};
  std::atomic<bool> host_opened_{false};  // Workbook_Open has finished
  std::atomic<bool> no_project_{false};   // SetNoProject
  io::MV host_info_;                      // contents of host.ready (macros, UDFs, events); protected by state_mu_
  // the @udf functions' help (Insert Function): from the host (signature, docstring's first line) and what
  // Application.MacroOptions set (description, category, argument descriptions), which wins; state_mu_
  struct UdfHelp {
    std::string args, desc, category;
    std::vector<std::pair<std::string, std::string>> arg_help;
    bool has_desc = false, has_cat = false, has_args = false;
    bool per_cell = false;  // it reads its calling cell: called for each cell
  };
  std::map<std::string, UdfHelp> udf_help_, udf_options_;
  void TakeUdfHelp(const io::MV& help);  // {NAME: {args, desc}} from host.ready / udf.register
  void PublishUdfHelp();                 // formula::SetUserFuncHelp from both
  std::wstring ctl_pipe_name_;
  std::thread ctl_thread_;
  std::atomic<bool> ctl_stop_{false};
  std::vector<std::thread> ctl_workers_;
  std::mutex ctl_mu_;

  std::mutex ui_mu_;
  std::deque<std::function<void(AppState&)>> ui_queue_;
  std::atomic<bool> ui_closed_{false};
  void WaitUi(std::future<io::MV>& fut, int timeout_s);
  bool HostStopping() const;  // this is the host's reader thread and StopHost is waiting for it

  std::mutex state_mu_;
  std::vector<ConsoleLine> console_;
  uint64_t console_base_ = 0;  // lines already removed from the front of console_ (console cursor = base + index)
  std::mutex job_mu_;
  std::map<int64_t, std::shared_ptr<std::promise<io::MV>>> jobs_;
  std::atomic<int64_t> next_job_{1};
  std::atomic<int> auto_dialogs_{0};  // > 0: answer MsgBox / InputBox automatically (while MCP runs a macro)

 public:
  bool AutoDialogs() const { return auto_dialogs_.load() > 0; }
  void LogLine(const std::string& text, int kind) { Log(text, kind); }

 private:
  // the focused sheet, copied by the UI thread every frame and after every UI request: ActiveSheet and
  // unqualified Range("A1") from Python read it without waiting for a UI frame (UINT32_MAX = not known yet)
  std::atomic<uint32_t> ui_focus_sheet_{UINT32_MAX};
  // the focus sheet's active cell, (row << 32) | column: the cell a relative name's text is shown and typed against
  std::atomic<uint64_t> ui_active_cell_{0};
  core::CellPos ActiveCellNow() const {
    const uint64_t v = ui_active_cell_.load();
    return core::CellPos{static_cast<uint32_t>(v >> 32), static_cast<uint32_t>(v & 0xFFFFFFFFu)};
  }
  void NoteActiveCell(AppState& s);
  std::vector<std::string> macros_, missing_;
  bool missing_dismissed_ = false;
  bool installing_ = false;    // env.install sent, no answer yet (a restart on success, env.install_failed otherwise)
  std::string install_error_;  // why the last install failed, shown on the bar with Try Again / Skip
  // code that failed on a missing package (Immediate window, Examples): run again once the package is installed
  std::string retry_code_;
  bool retry_shown_ = true;
  bool retry_after_restart_ = false;
  std::string python_version_;
  uint8_t event_sources_ = engine::kSrcUser | engine::kSrcPython;
  std::unordered_set<std::string> wanted_events_;  // handler names the host has (host.ready), under state_mu_
  std::map<std::string, std::unordered_set<uint32_t>> sheet_events_;  // Worksheet_* handler -> the sheets whose module has it
  struct PendingAsk {
    std::function<void(AppState&, bool)> done;
    std::chrono::steady_clock::time_point deadline;
    std::string name;
  };
  std::mutex ask_mu_;
  std::map<int64_t, PendingAsk> asks_;
  int64_t next_ask_ = 1;
  std::atomic<bool> before_close_done_{false};
  std::vector<std::string> pending_notes_;  // notifications to send once the host connects (already encoded)
  std::string instance_file_;
  std::string open_failed_, open_error_;
  bool startup_open_ = false;
  TopicSubs topic_subs_{[this](const io::MV& b) {
                          if (!host_ready_) return false;
                          Notify("topic.ticks", b);
                          return true;
                        },
                        [this] { return host_ready_.load(); }};
};

}  // namespace yc::app
