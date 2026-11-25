// SPDX-License-Identifier: Apache-2.0
// topic_subs.h - [YC] Application.Topics.Subscribe: every topic update (or the latest per topic) for the Python host
//
// Each subscription keeps a bounded queue (a ring: when full the oldest update is dropped and counted) filled as
// Topics.Push and replays deliver updates. A sender thread drains the queues in batches of at most max_batch
// updates and notifies the host ("topic.ticks"); the host runs each subscription's callback on its own thread, so a
// slow callback only delays its own subscription. Push never blocks on a subscriber.
#pragma once

#include "io/msgvalue.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace yc::app {

class TopicSubs {
 public:
  // send: notify the host with one batch; returns false when the host isn't there (the batch is kept)
  using SendFn = std::function<bool(const io::MV& batch)>;
  using ReadyFn = std::function<bool()>;
  TopicSubs(SendFn send, ReadyFn ready);
  ~TopicSubs();
  TopicSubs(const TopicSubs&) = delete;
  TopicSubs& operator=(const TopicSubs&) = delete;

  struct Options {
    bool conflate = false;     // the latest value per topic per cycle instead of every update
    size_t capacity = 1 << 20;  // updates kept while the host catches up
    size_t max_batch = 1000;    // updates per notification
  };
  int64_t Add(std::vector<std::string> topics, const Options& o);
  bool Remove(int64_t id);
  void Clear();  // the host went away: its subscriptions too

  // any thread: topic name (without the "QUOTE|" prefix), value, time in ns since the epoch
  struct Update {
    std::string topic;
    io::MV value;
    int64_t time_ns = 0;
  };
  void Feed(const std::vector<Update>& updates);
  bool Active() const { return count_.load(std::memory_order_relaxed) > 0; }
  // [{id, topics, conflate, queued, delivered, dropped, capacity}]
  io::MV Stats() const;

  static int64_t NowNs();

 private:
  struct Tick {
    std::string topic;
    io::MV value;
    int64_t time_ns = 0;
    uint64_t seq = 0;
  };
  struct Sub {
    int64_t id = 0;
    std::set<std::string> topics;
    Options opt;
    std::deque<Tick> queue;                 // every update
    std::map<std::string, Tick> latest;     // conflate: the newest per topic
    uint64_t seq = 0, delivered = 0, dropped = 0, dropped_sent = 0;
  };
  void Loop();

  SendFn send_;
  ReadyFn ready_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<int64_t, std::unique_ptr<Sub>> subs_;
  std::map<std::string, std::vector<Sub*>> by_topic_;
  std::atomic<int> count_{0};
  int64_t next_id_ = 1;
  bool stop_ = false, pending_ = false;
  std::thread th_;
};

}  // namespace yc::app
