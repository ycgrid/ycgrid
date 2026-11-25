// SPDX-License-Identifier: Apache-2.0
// topic_subs.cpp - [YC] Application.Topics.Subscribe (see topic_subs.h)
#include "topic_subs.h"

#include <algorithm>
#include <chrono>

namespace yc::app {

using io::MV;

int64_t TopicSubs::NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

TopicSubs::TopicSubs(SendFn send, ReadyFn ready) : send_(std::move(send)), ready_(std::move(ready)) {
  th_ = std::thread([this] { Loop(); });
}

TopicSubs::~TopicSubs() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

int64_t TopicSubs::Add(std::vector<std::string> topics, const Options& o) {
  std::lock_guard<std::mutex> lk(mu_);
  auto s = std::make_unique<Sub>();
  s->id = next_id_++;
  s->opt = o;
  s->opt.capacity = std::max<size_t>(1, o.capacity);
  s->opt.max_batch = std::max<size_t>(1, o.max_batch);
  for (auto& t : topics) {
    if (s->topics.insert(t).second) by_topic_[t].push_back(s.get());
  }
  const int64_t id = s->id;
  subs_[id] = std::move(s);
  count_.store(static_cast<int>(subs_.size()), std::memory_order_relaxed);
  cv_.notify_all();
  return id;
}

bool TopicSubs::Remove(int64_t id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = subs_.find(id);
  if (it == subs_.end()) return false;
  Sub* s = it->second.get();
  for (const auto& t : s->topics) {
    auto bt = by_topic_.find(t);
    if (bt == by_topic_.end()) continue;
    std::erase(bt->second, s);
    if (bt->second.empty()) by_topic_.erase(bt);
  }
  subs_.erase(it);
  count_.store(static_cast<int>(subs_.size()), std::memory_order_relaxed);
  return true;
}

void TopicSubs::Clear() {
  std::lock_guard<std::mutex> lk(mu_);
  subs_.clear();
  by_topic_.clear();
  count_.store(0, std::memory_order_relaxed);
}

void TopicSubs::Feed(const std::vector<Update>& updates) {
  if (!Active()) return;
  bool any = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& u : updates) {
      auto bt = by_topic_.find(u.topic);
      if (bt == by_topic_.end()) continue;
      for (Sub* s : bt->second) {
        Tick t{u.topic, u.value, u.time_ns, ++s->seq};
        if (s->opt.conflate) {
          s->latest[u.topic] = std::move(t);
        } else {
          if (s->queue.size() >= s->opt.capacity) {  // the ring is full: the oldest goes
            s->queue.pop_front();
            ++s->dropped;
          }
          s->queue.push_back(std::move(t));
        }
        any = true;
      }
    }
    if (any) pending_ = true;
  }
  if (any) cv_.notify_one();
}

io::MV TopicSubs::Stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  MV a = MV::Array();
  for (const auto& [id, s] : subs_) {
    MV x = MV::Map();
    x.Set("id", MV::Int(id));
    MV ts = MV::Array();
    for (const auto& t : s->topics) ts.Push(MV::Str(t));
    x.Set("topics", std::move(ts));
    x.Set("conflate", MV::Bool(s->opt.conflate));
    x.Set("queued", MV::Int(static_cast<int64_t>(s->opt.conflate ? s->latest.size() : s->queue.size())));
    x.Set("delivered", MV::Int(static_cast<int64_t>(s->delivered)));
    x.Set("dropped", MV::Int(static_cast<int64_t>(s->dropped)));
    x.Set("capacity", MV::Int(static_cast<int64_t>(s->opt.capacity)));
    a.Push(std::move(x));
  }
  return a;
}

void TopicSubs::Loop() {
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    // conflated subscriptions are sent at most every 20 ms (one cycle); others as soon as updates arrive
    if (subs_.empty()) cv_.wait(lk, [this] { return stop_ || pending_ || !subs_.empty(); });
    else cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return stop_ || pending_; });
    if (stop_) return;
    if (!pending_) continue;
    if (!ready_()) {  // no host yet: keep the updates (each ring stays bounded)
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      lk.lock();
      continue;
    }
    pending_ = false;
    bool more = true;
    while (more && !stop_) {
      more = false;
      std::vector<MV> batches;
      for (auto& [id, s] : subs_) {
        MV ticks = MV::Array();
        if (s->opt.conflate) {
          for (auto& [name, t] : s->latest) {
            MV row = MV::Array();
            row.Push(MV::Str(t.topic));
            row.Push(std::move(t.value));
            row.Push(MV::Int(t.time_ns));
            row.Push(MV::Int(static_cast<int64_t>(t.seq)));
            ticks.Push(std::move(row));
          }
          s->latest.clear();
        } else {
          const size_t n = std::min(s->queue.size(), s->opt.max_batch);
          ticks.a.reserve(n);
          for (size_t i = 0; i < n; ++i) {
            Tick& t = s->queue.front();
            MV row = MV::Array();
            row.a.reserve(4);
            row.Push(MV::Str(std::move(t.topic)));
            row.Push(std::move(t.value));
            row.Push(MV::Int(t.time_ns));
            row.Push(MV::Int(static_cast<int64_t>(t.seq)));
            ticks.Push(std::move(row));
            s->queue.pop_front();
          }
          if (!s->queue.empty()) more = true;
        }
        if (ticks.a.empty() && s->dropped == s->dropped_sent) continue;
        s->delivered += ticks.a.size();
        s->dropped_sent = s->dropped;
        MV b = MV::Map();
        b.Set("sub", MV::Int(id));
        b.Set("ticks", std::move(ticks));
        b.Set("dropped", MV::Int(static_cast<int64_t>(s->dropped)));
        batches.push_back(std::move(b));
      }
      if (batches.empty()) break;
      lk.unlock();  // Feed keeps going while the batches are written
      for (const auto& b : batches) send_(b);
      lk.lock();
    }
  }
}

}  // namespace yc::app
