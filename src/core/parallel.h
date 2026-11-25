// SPDX-License-Identifier: Apache-2.0
// parallel.h - a small persistent worker pool (PLAN §7.3: parallel recalculation)
//
// Standard C++ only (core modules don't depend on Win32, hard rule 8). The workers sleep on a condition variable
// between jobs, so an idle pool costs nothing.
#pragma once

#include <cstddef>
#include <functional>

namespace yc::core {

// number of threads ParallelFor uses, the calling thread included (1 = no worker threads): the pool's size, or the
// limit set below when it is smaller
size_t WorkerCount();
// the pool's own size (one thread per core but two, the calling thread included)
size_t PoolSize();
// at most this many threads take part in a ParallelFor, the calling thread included (0 = the whole pool; 1 = none
// but the caller): Application.MultiThreadedCalculation's ThreadCount, and Enabled = False
void SetWorkerLimit(size_t n);

// runs fn(begin, end) over [0, n) in chunks of `grain` items on the worker threads and the calling thread, and returns
// when every chunk is done. fn must not throw (catch inside) and must not call ParallelFor; jobs from different threads
// run one after another.
void ParallelFor(size_t n, size_t grain, const std::function<void(size_t begin, size_t end)>& fn);

}  // namespace yc::core
