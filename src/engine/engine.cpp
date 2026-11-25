// SPDX-License-Identifier: Apache-2.0
#include "engine/engine.h"
#include "formula/datetime.h"

#include "formula/numfmt.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace yc::engine {

namespace {
thread_local bool t_live = false;  // set by Engine::LiveScope
}

Engine::LiveScope::LiveScope() : prev_(t_live) { t_live = true; }
thread_local Engine::BlockSink t_blocks;  // set by Engine::BlockScope
Engine::BlockScope::BlockScope(BlockSink sink) : prev_(t_blocks) { t_blocks = std::move(sink); }
Engine::BlockScope::~BlockScope() { t_blocks = std::move(prev_); }
Engine::BlockSink Engine::CurrentBlockSink() { return t_blocks; }
Engine::LiveScope::~LiveScope() { t_live = prev_; }

void Engine::SetPublishInterval(int ms) {
  publish_interval_ms_ = std::clamp(ms, 0, 5000);
  cv_.notify_one();
}

// ---- Implementation of snapshot.h ----

std::string FormulaTextOf(const model::Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c, bool r1c1) {
  const model::Sheet* s = wb.SheetById(sheet);
  if (!s) return {};
  const auto* f = static_cast<const formula::CompiledFormula*>(wb.formulas->Get(s->GetFormula(r, c)));
  if (!f) return {};
  WorkbookResolver res(wb);
  return "=" + (r1c1 ? formula::ToR1C1(f->root, core::CellPos{r, c}, &res) : formula::ToA1(f->root, core::CellPos{r, c}, &res));
}

std::string DataTableText(const model::Sheet& s, uint32_t r, uint32_t c) {
  for (const auto& t : *s.data_tables) {
    if (!t.Body().Contains(r, c)) continue;
    return "{=TABLE(" + (t.has_row ? core::CellA1(t.row_r, t.row_c) : std::string()) + "," + (t.has_col ? core::CellA1(t.col_r, t.col_c) : std::string()) + ")}";
  }
  return {};
}

std::string InputTextOf(const model::Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c, bool r1c1) {
  std::string f = FormulaTextOf(wb, sheet, r, c, r1c1);
  if (!f.empty()) return f;
  const model::Sheet* s = wb.SheetById(sheet);
  if (s && !s->data_tables->empty()) {
    if (std::string t = DataTableText(*s, r, c); !t.empty()) return t;
  }
  if (!s) return {};
  const model::Style& st = wb.styles->Get(s->GetStyle(r, c));
  return InputTextOf(s->GetValue(r, c), wb.styles->NumberFormat(st));
}

const model::Style& StyleOf(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c) {
  return wb.styles->Get(s.GetStyle(r, c));
}

namespace {

struct Releaser {
  std::mutex mu;
  std::condition_variable cv;       // work arrived
  std::condition_variable idle_cv;  // the worker has freed everything it was given
  std::vector<std::shared_ptr<void>> q;
  bool busy = false;
  Releaser() {
    std::thread([this] {
      ::SetThreadDescription(::GetCurrentThread(), L"yc-engine-release");
      std::unique_lock<std::mutex> lk(mu);
      while (true) {
        cv.wait(lk, [this] { return !q.empty(); });
        auto batch = std::move(q);
        q.clear();
        busy = true;
        lk.unlock();
        batch.clear();  // the release happens here
        lk.lock();
        busy = false;
        if (q.empty()) idle_cv.notify_all();
      }
    }).detach();
  }
};

Releaser& TheReleaser() {
  static Releaser* r = new Releaser();  // never destroyed: the thread runs until the process ends
  return *r;
}

}  // namespace

// what the documents drop (undo entries a new action ends) is freed on this thread: the last owner of a pasted block
// of millions of cells takes a second to free, and the action that drops it (often a Call the UI waits on) shouldn't
void ReleaseOnWorker(std::shared_ptr<void> held) {
  if (!held) return;
  Releaser& r = TheReleaser();
  {
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.q.size() >= 64) return;  // bounded (hard rule 6): the worker is behind, this one is freed here
    r.q.push_back(std::move(held));
  }
  r.cv.notify_one();
}

void WaitForReleases() {
  Releaser& r = TheReleaser();
  std::unique_lock<std::mutex> lk(r.mu);
  r.idle_cv.wait(lk, [&r] { return r.q.empty() && !r.busy; });
}

// ---- Engine ----

namespace {
// The workbook's date system, set before a Document is made of it: its constructor calculates every formula, and
// DATE, YEAR and the rest read the date system then. Set after it, a 1904 workbook opened in a 1900 session kept
// =DATE(2024,1,1) at 45292 (2028-01-02 in its own system) until something calculated it again
model::Workbook WithItsDateSystem(model::Workbook wb) {
  formula::SetDate1904(wb.date1904);
  return wb;
}
}  // namespace

Engine::Engine(model::Workbook wb) : doc_(std::make_unique<Document>(WithItsDateSystem(std::move(wb)))) {
  thread_id_ = std::this_thread::get_id();
  doc_->block_counter = &protect_blocks_;
  doc_->block_reason = &block_reason_;
  doc_->release_later = ReleaseOnWorker;
  doc_->interrupt = &interrupt_;
  formula::SetDate1904(doc_->wb().date1904);  // the workbook's date system, for every thread
  Publish(doc_->TakeChanges());
}

Engine::~Engine() { Stop(); }

void Engine::Start() {
  if (thread_.joinable()) return;
  stop_ = false;
  // the caller stops being the engine thread now, not when the new thread first runs: a Call() made right after
  // Start() ran on the caller, beside the engine thread, and its change was never published
  thread_id_ = std::thread::id();
  thread_ = std::thread([this] {
    thread_id_ = std::this_thread::get_id();
    ::SetThreadDescription(::GetCurrentThread(), L"yc-engine");
    Loop();
  });
  thread_id_ = thread_.get_id();
}

void Engine::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  thread_id_ = std::this_thread::get_id();
}

void Engine::Post(Task fn) { PostThen(std::move(fn), nullptr); }

void Engine::PostThen(Task fn, std::shared_ptr<std::function<void()>> after) {
  if (!thread_.joinable()) {
    // not started yet (initialization, tests): run directly
    fn(*doc_);
    doc_->Recalc();
    Publish(doc_->TakeChanges());
    if (after && *after) (*after)();
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(Item{std::move(fn), t_live, {}, t_blocks, std::move(after)});
  }
  cv_.notify_one();
}

void Engine::PostUser(std::string label, Task fn) {
  user_posted_.fetch_add(1, std::memory_order_acq_rel);
  Post([this, label = std::move(label), fn = std::move(fn)](Document& d) mutable {
    d.Begin(std::move(label));
    try {
      fn(d);
    } catch (...) {
      d.Commit();
      ++user_done_;
      throw;
    }
    d.Commit();
    ++user_done_;
  });
}

void Engine::Sync() {
  if (OnEngineThread() || !thread_.joinable()) return;
  auto prom = std::make_shared<std::promise<void>>();
  auto fut = prom->get_future();
  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(Item{[](Document&) {}, false, [prom] { prom->set_value(); }, {}});
  }
  cv_.notify_one();
  fut.wait();
}

void Engine::Replace(model::Workbook wb) {
  Post([this, w = std::make_shared<model::Workbook>(std::move(wb))](Document&) mutable {
    // application-level options survive opening another workbook
    const uint64_t shift_seq = doc_->shift_seq;
    const bool auto_calc = doc_->auto_calc, auto_tables = doc_->auto_data_tables, excel15 = doc_->excel_15_digit_input, iterate = doc_->iterate;
    const int max_iterations = doc_->max_iterations;
    const std::string audit_user = doc_->audit_user;
    const double max_change = doc_->max_change;
    doc_ = std::make_unique<Document>(WithItsDateSystem(std::move(*w)));
    doc_->auto_calc = auto_calc;
    doc_->auto_data_tables = auto_tables;
    doc_->shift_seq = shift_seq;  // the UI goes on from the shifts it has applied
    doc_->excel_15_digit_input = excel15;
    doc_->iterate = iterate;
    doc_->max_iterations = max_iterations;
    doc_->audit_user = audit_user;
    doc_->max_change = max_change;
    // a file saved with its iterative calculation settings sets them, as opening it in Excel does
    if (doc_->wb().calc_iterate >= 0) {
      doc_->iterate = doc_->wb().calc_iterate > 0;
      doc_->max_iterations = doc_->wb().calc_max_iterations;
      doc_->max_change = doc_->wb().calc_max_change;
      if (doc_->iterate) doc_->RecalcAll();  // the file's cycles are calculated with them
    }
    if (doc_->wb().calc_mode >= 0) {  // a file saved in Manual opens in Manual
      doc_->auto_calc = doc_->wb().calc_mode != 1;
      doc_->auto_data_tables = doc_->wb().calc_mode != 2;
    }
    doc_->provider = provider_;
    doc_->net = net_;
    doc_->block_counter = &protect_blocks_;
    doc_->block_reason = &block_reason_;
    doc_->release_later = ReleaseOnWorker;
    doc_->interrupt = &interrupt_;
    formula::SetDate1904(doc_->wb().date1904);  // the new workbook's date system
  });
}

void Engine::SetNet(NetProvider* n) {
  Post([this, n](Document& d) {
    net_ = n;
    d.net = n;
  });
}

void Engine::SetProvider(ExternalProvider* p) {
  Post([this, p](Document& d) {
    provider_ = p;
    d.provider = p;
    d.ProviderChanged();  // stream formulas subscribe again; Python functions are asked again when the host is ready
  });
}

void Engine::Loop() {
  using clock = std::chrono::steady_clock;
  std::deque<Item> batch;
  std::vector<std::function<void()>> waiters;  // Sync() callers waiting for the next publish
  bool dirty = false;   // commands ran since the last publish
  bool urgent = false;  // one of them wasn't live: publish at once
  clock::time_point last_pub = clock::now() - std::chrono::hours(1);
  while (true) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      if (!batch.empty()) {
        // commands left from the previous cycle (it stopped to publish): take the new ones too, in order
        for (auto& x : queue_) batch.push_back(std::move(x));
        queue_.clear();
      } else {
        // wake up for: new commands, the publish interval (live changes waiting), held-back bound-table values
        const int64_t flush = doc_->BindNextFlush();
        clock::time_point until = clock::time_point::max();
        if (dirty) until = last_pub + std::chrono::milliseconds(publish_interval_ms_.load());
        if (flush) until = std::min(until, clock::now() + std::chrono::nanoseconds(std::max<int64_t>(0, flush - PerfCounterNs())));
        if (queue_.empty()) busy_since_ticks_.store(0, std::memory_order_relaxed);
        if (until == clock::time_point::max()) {
          cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
        } else {
          cv_.wait_until(lk, until, [&] { return stop_ || !queue_.empty(); });
        }
        if (stop_ && queue_.empty()) return;
        batch.swap(queue_);
      }
    }
    if (!batch.empty() && !busy_since_ticks_.load(std::memory_order_relaxed)) busy_since_ticks_.store(clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    if (const int64_t flush = doc_->BindNextFlush(); flush && PerfCounterNs() >= flush) {
      doc_->python_call = true;  // held-back values of Python's bound tables (UserInterfaceOnly lets them through)
      doc_->BindFlush(PerfCounterNs());
      dirty = true;
    }
    while (!batch.empty()) {
      Item it = std::move(batch.front());
      batch.pop_front();
      doc_->python_call = it.live;  // commands from Python: UserInterfaceOnly protection lets them through
      const auto t_task = clock::now();
      const uint32_t blocks0 = doc_->protect_blocks();
      const Document* doc0 = doc_.get();
      try {
        it.fn(*doc_);
      } catch (const std::exception&) {
        // a failed command doesn't affect the engine; the error goes back through the caller's promise
      }
      // this command's own refusals only: a command that replaced the workbook (Replace: open, new, close) starts a
      // new count at 0, which isn't "minus the old workbook's refusals" (that wrapped around and reported a refusal
      // made long before, e.g. Workbook.Close after a refused write)
      const uint32_t blocks1 = doc_->protect_blocks();
      const uint32_t refused = doc_.get() == doc0 ? blocks1 - blocks0 : blocks1;
      if (it.blocks && refused) it.blocks->fetch_add(refused);
      if (it.after && *it.after) (*it.after)();  // Call(): its caller wakes up with the refusals counted
      us_tasks += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t_task).count();
      ++tasks_run;
      dirty = true;
      urgent = urgent || !it.live;
      if (it.on_published) waiters.push_back(std::move(it.on_published));
      // a continuous stream of commands mustn't keep the screen waiting: publish when the interval is due (the rest
      // of the batch runs after the publish, in order)
      {
        const int iv = publish_interval_ms_.load();
        if (iv > 0 && clock::now() - last_pub >= std::chrono::milliseconds(iv)) break;
      }
      // new commands arrived while one was running: handle them together before publishing (merged publish cycle)
      if (batch.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!queue_.empty() && batch.size() < 4096) batch.swap(queue_);
      }
    }
    const int interval = publish_interval_ms_.load();
    const bool due = clock::now() - last_pub >= std::chrono::milliseconds(interval);
    if (!dirty || !(urgent || !waiters.empty() || interval <= 0 || due)) continue;
    const auto t_rc = clock::now();
    if (doc_->auto_calc) doc_->Recalc();
    else doc_->CalcEntered();
    // a calculation a key interrupted (CalculationInterruptKey) goes on in automatic calculation once what the person
    // did meanwhile (already queued) has run: the cells it didn't reach never stay behind
    if (doc_->auto_calc && doc_->CalcInterrupted()) Post([](Document&) {});
    const auto t_pub = clock::now();
    Publish(doc_->TakeChanges());
    last_pub = clock::now();
    us_recalc += std::chrono::duration_cast<std::chrono::microseconds>(t_pub - t_rc).count();
    us_publish += std::chrono::duration_cast<std::chrono::microseconds>(last_pub - t_pub).count();
    last_pub_ticks_.store(last_pub.time_since_epoch().count(), std::memory_order_relaxed);
    dirty = urgent = false;
    for (auto& w : waiters) w();
    waiters.clear();
  }
}

void Engine::Publish(std::vector<CellChange> changes) {
  auto s = std::make_shared<Snapshot>();
  doc_->wb().FinalizeForSnapshot();
  s->wb = doc_->wb();
  // what a save writes: the calculation settings the workbook is calculated with (Excel keeps them in the file)
  s->wb.calc_iterate = doc_->iterate ? 1 : 0;
  s->wb.calc_max_iterations = doc_->max_iterations;
  s->wb.calc_max_change = doc_->max_change;
  s->wb.calc_mode = static_cast<int8_t>(!doc_->auto_calc ? 1 : doc_->auto_data_tables ? 0 : 2);
  s->version = ++version_;
  s->stats = doc_->last_stats();
  s->can_undo = doc_->CanUndo();
  s->can_redo = doc_->CanRedo();
  s->undo_label = doc_->UndoLabel();
  s->redo_label = doc_->RedoLabel();
  s->undo_labels = doc_->UndoLabels(false, 100);
  s->redo_labels = doc_->UndoLabels(true, 100);
  s->undo_depth = static_cast<uint32_t>(doc_->UndoDepth());
  s->steps_made = doc_->StepsMade();
  s->redo_depth = static_cast<uint32_t>(doc_->RedoDepth());
  s->user_done = user_done_;
  s->circular = doc_->circular_cells.size();
  s->first_circular = doc_->circular_cells.empty() ? 0 : doc_->circular_cells.front();
  // the status bar names a circular cell of the sheet shown (Excel), so a few per sheet are passed on
  s->spill_blocked.clear();
  for (const auto& [k, want] : doc_->spill_blocked()) {
    if (s->spill_blocked.size() >= 4096) break;
    s->spill_blocked.emplace_back(k, want);
  }
  s->circular_cells.assign(doc_->circular_cells.begin(), doc_->circular_cells.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(doc_->circular_cells.size(), 4096)));
  s->calculating = doc_->has_pending();
  s->manual_calc = !doc_->auto_calc;
  s->semiauto_calc = doc_->auto_calc && !doc_->auto_data_tables;
  {
    // copied only when it changed (most snapshots share the previous copy)
    static thread_local std::shared_ptr<const std::vector<StructShift>> last;
    if (!last || (doc_->shift_log.empty() ? !last->empty() : (last->empty() || last->back().seq != doc_->shift_log.back().seq)))
      last = std::make_shared<const std::vector<StructShift>>(doc_->shift_log);
    s->shift_log = last;
  }
  s->protect_blocks = doc_->protect_blocks();
  s->block_sheet = doc_->block_sheet();
  s->block_cells = doc_->block_cells();
  s->block_range = doc_->block_range();
  s->block_pivot = doc_->block_pivot();
  s->block_read_only = doc_->block_read_only();
  s->block_table = doc_->block_table();
  s->block_off_sheet = doc_->block_off_sheet();
  s->block_too_large = doc_->block_too_large();
  s->block_table_merge = doc_->block_table_merge();
  s->block_merge_size = doc_->block_merge_size();
  s->block_table_shift = doc_->block_table_shift();
  s->block_pivot_shift = doc_->block_pivot_shift();
  s->block_filter_shift = doc_->block_filter_shift();
  s->block_array = doc_->block_array();
  s->block_python = doc_->block_python();
  s->block_param = doc_->block_param();
  doc_->TakeInputTimes(&s->input_newest_ns, &s->input_oldest_ns);
  s->bind_cells = doc_->bind_cells_written();
  if (!doc_->wb().bound_tables->empty()) s->bind_times = doc_->AllBoundRowTimes();
  doc_->TakeSheetEvents(&s->calc_sheets, &s->added_sheets);
  std::shared_ptr<const Snapshot> cs = s;
  current_.store(cs, std::memory_order_release);
  if (on_changes && (!changes.empty() || !cs->calc_sheets.empty() || !cs->added_sheets.empty())) on_changes(changes, *cs);
  if (on_publish) on_publish();
}

}  // namespace yc::engine
