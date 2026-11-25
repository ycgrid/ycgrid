// SPDX-License-Identifier: Apache-2.0
#include "core/parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace yc::core {
namespace {

std::atomic<size_t> g_limit{0};  // SetWorkerLimit

class Pool {
 public:
  explicit Pool(size_t workers) {
    for (size_t i = 0; i < workers; ++i) threads_.emplace_back([this, i] { WorkerLoop(i); });
  }

  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
  }

  size_t size() const { return threads_.size() + 1; }

  void Run(size_t n, size_t grain, const std::function<void(size_t, size_t)>& fn) {
    std::lock_guard<std::mutex> one_job(run_mu_);
    {
      std::lock_guard<std::mutex> lk(mu_);
      fn_ = &fn;
      n_ = n;
      grain_ = std::max<size_t>(1, grain);
      next_ = 0;
      busy_ = threads_.size();
      ++generation_;
    }
    wake_.notify_all();
    Work();
    std::unique_lock<std::mutex> lk(mu_);
    done_.wait(lk, [&] { return busy_ == 0; });
    fn_ = nullptr;
  }

 private:
  void Work() {
    while (true) {
      const size_t b = next_.fetch_add(grain_);
      if (b >= n_) return;
      (*fn_)(b, std::min(n_, b + grain_));
    }
  }

  void WorkerLoop(size_t index) {
    uint64_t seen = 0;
    while (true) {
      {
        std::unique_lock<std::mutex> lk(mu_);
        wake_.wait(lk, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
      }
      // a limit leaves the workers past it out of this job (the calling thread is one of the limit's threads)
      const size_t limit = g_limit.load(std::memory_order_relaxed);
      if (limit == 0 || index + 1 < limit) Work();
      std::lock_guard<std::mutex> lk(mu_);
      if (--busy_ == 0) done_.notify_one();
    }
  }

  std::vector<std::thread> threads_;
  std::mutex run_mu_;  // one job at a time
  std::mutex mu_;
  std::condition_variable wake_, done_;
  bool stop_ = false;
  uint64_t generation_ = 0;
  size_t busy_ = 0;
  // the current job (written under mu_ before the workers are woken)
  const std::function<void(size_t, size_t)>* fn_ = nullptr;
  size_t n_ = 0, grain_ = 1;
  std::atomic<size_t> next_{0};
};

Pool& GetPool() {
  // one thread per core, the calling thread included; a few cores stay free for the UI and the Python host
  static Pool pool([] {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<size_t>(hw > 2 ? hw - 2 : 0);
  }());
  return pool;
}

}  // namespace

size_t PoolSize() { return GetPool().size(); }

size_t WorkerCount() {
  const size_t limit = g_limit.load(std::memory_order_relaxed), n = GetPool().size();
  return limit == 0 ? n : std::min(limit, n);
}

void SetWorkerLimit(size_t n) { g_limit.store(n, std::memory_order_relaxed); }

void ParallelFor(size_t n, size_t grain, const std::function<void(size_t, size_t)>& fn) {
  if (n == 0) return;
  Pool& pool = GetPool();
  if (WorkerCount() == 1 || n <= grain) {
    fn(0, n);
    return;
  }
  pool.Run(n, grain, fn);
}

}  // namespace yc::core
