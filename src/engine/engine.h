// SPDX-License-Identifier: Apache-2.0
// engine.h - engine thread (PLAN §4)
//
// All writes run in order on the engine thread (single writer). The UI, the YCP server and data feeds send commands with Post / Call;
// after a batch the engine recalculates, publishes a new snapshot (atomic pointer swap) and calls on_publish to wake the UI.
//
// Publishing (PLAN §2.1): commands from the UI publish at once. Commands sent inside a LiveScope (Python, data feeds) are
// throttled: the first change after a quiet period publishes at once, a continuous stream publishes at most once per
// publish interval, so a flood of market data can't keep the engine recalculating and the UI redrawing nonstop.
#pragma once

#include "engine/document.h"
#include "engine/snapshot.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

namespace yc::engine {

// Frees `held` on a background thread shared by every engine (what a Document drops through release_later).
void ReleaseOnWorker(std::shared_ptr<void> held);
// Waits until that thread has freed everything handed to it so far (tests that watch the tile pool: it frees tiles
// at any moment otherwise).
void WaitForReleases();

class Engine {
 public:
  using Task = std::function<void(Document&)>;

  explicit Engine(model::Workbook wb = model::Workbook::NewDefault());
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void Start();
  void Stop();

  std::shared_ptr<const Snapshot> Current() const { return current_.load(std::memory_order_acquire); }

  // asynchronous command
  void Post(Task fn);
  // user action: wrapped in one undo step (label is shown in the Undo tooltip)
  void PostUser(std::string label, Task fn);
  // user actions posted that s doesn't show yet: one still queued will clear the redo list (F4 and Ctrl+Y decide
  // between Redo and Repeat on what the engine will have done, not on a frame that is behind)
  bool UserActionsPending(const Snapshot& s) const { return user_posted_.load(std::memory_order_acquire) != s.user_done; }
  // synchronous call (YCP reads/writes, dialogs that need a result); runs directly when called on the engine thread
  template <class F>
  auto Call(F&& f) -> std::invoke_result_t<F, Document&>;
  // wait until every command sent so far has been handled and published (tests, before saving)
  // seconds the engine has been working without a break (0 when it waits for work): the UI hands work over instead of
  // waiting for it when this is long
  // stops the calculation in progress at its next cell (Application.CalculationInterruptKey, CheckAbort); the cells it
  // didn't reach stay pending. Nothing happens when the engine isn't calculating.
  void InterruptCalc() {
    if (busy_since_ticks_.load(std::memory_order_relaxed) != 0) interrupt_.store(true, std::memory_order_relaxed);
  }
  double BusySeconds() const {
    const int64_t t = busy_since_ticks_.load(std::memory_order_relaxed);
    if (!t) return 0;
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::chrono::duration<double>(std::chrono::steady_clock::duration(now - t)).count();
  }
  void Sync();

  // minimum time between publishes caused by live commands (ms; 0 = publish after every batch)
  void SetPublishInterval(int ms);
  int PublishInterval() const { return publish_interval_ms_.load(); }
  // commands posted by this thread while the scope lives count as live data (the Python bridge wraps its requests in one)
  // the protection refusals of the commands posted while it is alive (from this thread, and from the UI thread on
  // behalf of this request) are counted in sink: a request reports E_PROTECTED for its own refusals only, not for
  // the user's or another connection's running at the same time
  using BlockSink = std::shared_ptr<std::atomic<uint32_t>>;
  class BlockScope {
   public:
    explicit BlockScope(BlockSink sink);
    ~BlockScope();
    BlockScope(const BlockScope&) = delete;
    BlockScope& operator=(const BlockScope&) = delete;

   private:
    BlockSink prev_;
  };
  static BlockSink CurrentBlockSink();

  class LiveScope {
   public:
    LiveScope();
    ~LiveScope();
    LiveScope(const LiveScope&) = delete;
    LiveScope& operator=(const LiveScope&) = delete;

   private:
    bool prev_;
  };

  // load a whole new workbook (opening a file)
  void Replace(model::Workbook wb);
  // provider of UDFs / streams (the Python host); kept when the workbook changes
  void SetProvider(ExternalProvider* p);
  void SetNet(NetProvider* n);  // WEBSERVICE downloads; kept when the workbook changes

  // callbacks (called on the engine thread; must not block for long)
  std::function<void()> on_publish;
  std::function<void(const std::vector<CellChange>&, const Snapshot&)> on_changes;

  bool OnEngineThread() const { return std::this_thread::get_id() == thread_id_.load(); }
  // a long task of live data can stop when this turns true and post the rest: the screen gets its publish on time
  bool PublishDue() const {
    const int iv = publish_interval_ms_.load();
    return iv > 0 && std::chrono::steady_clock::now().time_since_epoch().count() - last_pub_ticks_.load(std::memory_order_relaxed) >=
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(iv)).count();
  }
  uint64_t version() const { return version_.load(); }

 private:
  void Loop();
  void Publish(std::vector<CellChange> changes);

  std::unique_ptr<Document> doc_;
  ExternalProvider* provider_ = nullptr;
  NetProvider* net_ = nullptr;
  std::atomic<std::shared_ptr<const Snapshot>> current_;
  std::atomic<uint64_t> version_{0};

  struct Item {
    Task fn;
    bool live = false;               // from a LiveScope: may wait for the publish interval
    std::function<void()> on_published;  // Sync(): called after the publish that includes this command
    BlockSink blocks;                    // from a BlockScope: counts this command's protection refusals
    std::shared_ptr<std::function<void()>> after;  // Call(): hands the result back once the refusals are counted
  };
  // Post, with `after` run on the engine thread right after the command's refusals are added to its BlockScope:
  // Call() answers from there, so a caller that reads its sink when Call returns sees every refusal of the call
  void PostThen(Task fn, std::shared_ptr<std::function<void()>> after);

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  std::atomic<int> publish_interval_ms_{100};
  std::atomic<int64_t> last_pub_ticks_{0};  // steady_clock ticks of the last publish (PublishDue)
  std::atomic<bool> interrupt_{false};  // InterruptCalc
  std::atomic<int64_t> busy_since_ticks_{0};  // steady_clock ticks since the engine has been working (0 = waiting)
  std::atomic<uint64_t> user_posted_{0};  // PostUser calls so far (any thread)
  uint64_t user_done_ = 0;                // of them, the ones done (engine thread only; published in Snapshot::user_done)

 public:
  // where the engine thread's time goes (microseconds, totals since start): commands, recalculation, publishing
  std::atomic<int64_t> us_tasks{0}, us_recalc{0}, us_publish{0}, tasks_run{0};

 private:
  std::atomic<uint32_t> protect_blocks_{0};  // operations refused by sheet / workbook protection (Python reports E_PROTECTED)
  std::atomic<int> block_reason_{0};         // BlockReason of the last refusal

 public:
  uint32_t protect_blocks() const { return protect_blocks_.load(std::memory_order_relaxed); }
  int block_reason() const { return block_reason_.load(std::memory_order_relaxed); }

 private:
  bool stop_ = false;
  std::thread thread_;
  std::atomic<std::thread::id> thread_id_;
};

template <class F>
auto Engine::Call(F&& f) -> std::invoke_result_t<F, Document&> {
  using R = std::invoke_result_t<F, Document&>;
  if (OnEngineThread() || !thread_.joinable()) return f(*doc_);
  auto prom = std::make_shared<std::promise<R>>();
  auto fut = prom->get_future();
  // the answer is handed back after the engine counted this call's refusals (PostThen), not from inside the
  // command: the Python bridge reads the count as soon as Call returns, and a write refused by protection was
  // sometimes reported as done
  auto answer = std::make_shared<std::function<void()>>();
  PostThen([prom, answer, fn = std::forward<F>(f)](Document& d) mutable {
    try {
      if constexpr (std::is_void_v<R>) {
        fn(d);
        *answer = [prom] { prom->set_value(); };
      } else {
        auto v = std::make_shared<R>(fn(d));
        *answer = [prom, v] { prom->set_value(std::move(*v)); };
      }
    } catch (...) {
      *answer = [prom, e = std::current_exception()] { prom->set_exception(e); };
    }
  }, answer);
  return fut.get();
}

}  // namespace yc::engine
