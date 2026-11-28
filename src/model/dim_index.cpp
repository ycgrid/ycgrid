// SPDX-License-Identifier: Apache-2.0
#include "model/dim_index.h"

#include <algorithm>

namespace yc::model {

size_t DimIndex::Find(int64_t index) const {
  return static_cast<size_t>(std::lower_bound(idx_.begin(), idx_.end(), index) - idx_.begin());
}

int32_t DimIndex::Size(int64_t index) const {
  const size_t j = Find(index);
  if (j < idx_.size() && idx_[j] == index) return size_[j];
  return default_;
}

bool DimIndex::IsCustom(int64_t index) const {
  const size_t j = Find(index);
  return j < idx_.size() && idx_[j] == index && size_[j] != 0 && !Stale(j);
}

void DimIndex::Compact() {
  size_t k = 0;
  for (size_t j = 0; j < idx_.size(); ++j) {
    if (Stale(j)) continue;
    idx_[k] = idx_[j];
    size_[k] = size_[j];
    saved_[k] = saved_[j];
    ++k;
  }
  idx_.resize(k);
  size_.resize(k);
  saved_.resize(k);
  stale_ = 0;
  dirty_ = true;
}

void DimIndex::Set(int64_t index, int32_t size) {
  const size_t j = Find(index);
  const bool exists = j < idx_.size() && idx_[j] == index;
  if (size == default_) {
    if (exists && !Stale(j)) {  // left in place, dropped by Compact once half the entries are stale
      size_[j] = default_;
      saved_[j] = -1;
      if (++stale_ > 64 && stale_ * 2 > idx_.size()) Compact();
    }
  } else if (exists) {
    if (Stale(j)) --stale_;
    size_[j] = size;
    saved_[j] = 0;
  } else {
    idx_.insert(idx_.begin() + static_cast<std::ptrdiff_t>(j), index);
    size_.insert(size_.begin() + static_cast<std::ptrdiff_t>(j), size);
    saved_.insert(saved_.begin() + static_cast<std::ptrdiff_t>(j), 0);
  }
  dirty_ = true;
}

void DimIndex::Hide(int64_t index, bool hidden) {
  const size_t j = Find(index);
  const bool exists = j < idx_.size() && idx_[j] == index;
  if (hidden) {
    const int32_t cur = exists ? size_[j] : default_;
    if (cur == 0) return;
    if (exists) {
      if (Stale(j)) --stale_;
      saved_[j] = cur;
      size_[j] = 0;
    } else {
      idx_.insert(idx_.begin() + static_cast<std::ptrdiff_t>(j), index);
      size_.insert(size_.begin() + static_cast<std::ptrdiff_t>(j), 0);
      saved_.insert(saved_.begin() + static_cast<std::ptrdiff_t>(j), default_);
    }
  } else if (exists && size_[j] == 0) {
    const int32_t restore = saved_[j] ? saved_[j] : default_;
    Set(index, restore);
  }
  dirty_ = true;
}

void DimIndex::Rebuild() const {
  before_.resize(idx_.size());
  int64_t acc = 0;
  for (size_t j = 0; j < idx_.size(); ++j) {
    before_[j] = acc;
    acc += static_cast<int64_t>(size_[j]) - default_;
  }
  dirty_ = false;
}

int64_t DimIndex::Offset(int64_t index) const {
  if (dirty_) Rebuild();
  const size_t j = Find(index);
  int64_t delta = 0;
  if (j > 0) delta = before_[j - 1] + (static_cast<int64_t>(size_[j - 1]) - default_);
  return index * default_ + delta;
}

int64_t DimIndex::IndexAt(int64_t px) const {
  if (px <= 0) return 0;
  if (dirty_) Rebuild();
  size_t lo = 0, hi = idx_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int64_t off = idx_[mid] * default_ + before_[mid];
    if (off <= px) lo = mid + 1;
    else hi = mid;
  }
  if (lo == 0) return px / default_;
  const size_t j = lo - 1;
  const int64_t off = idx_[j] * default_ + before_[j];
  if (px < off + size_[j]) return idx_[j];
  return idx_[j] + 1 + (px - (off + size_[j])) / default_;
}

int64_t DimIndex::NextVisible(int64_t index, int dir, int64_t limit) const {
  while (index >= 0 && index < limit) {
    if (Size(index) > 0) return index;
    index += dir;
  }
  return -1;
}

void DimIndex::SetLevel(int64_t index, int level) {
  auto it = levels_.find(index);
  if (it != levels_.end()) --level_count_[static_cast<size_t>(Slot(it->second))];
  if (level <= 0) {
    if (it != levels_.end()) levels_.erase(it);
    return;
  }
  const uint8_t l = static_cast<uint8_t>(level > kMaxOutline ? kMaxOutline : level);
  if (it != levels_.end()) it->second = l;
  else levels_.emplace(index, l);
  ++level_count_[l];
}

int DimIndex::MaxLevel() const {
  for (int l = kMaxOutline; l > 0; --l) {
    if (level_count_[static_cast<size_t>(l)] > 0) return l;
  }
  return 0;
}

void DimIndex::CountLevels() {
  level_count_.fill(0);
  for (const auto& [i, l] : levels_) ++level_count_[static_cast<size_t>(Slot(l))];
}

void DimIndex::Insert(int64_t at, int64_t count) {
  for (auto& i : idx_) {
    if (i >= at) i += count;
  }
  if (!levels_.empty()) {
    // inserted rows inside a group join it (the level of the row above, like Excel)
    std::map<int64_t, uint8_t> o;
    for (const auto& [i, l] : levels_) o.emplace_hint(o.end(), i >= at ? i + count : i, l);  // in order: no search
    const int inherit = at > 0 && Level(at - 1) && Level(at) ? std::min(Level(at - 1), Level(at)) : 0;
    if (inherit) {
      for (int64_t k = at; k < at + count; ++k) o[k] = static_cast<uint8_t>(inherit);
    }
    levels_ = std::move(o);
    CountLevels();
  }
  if (!collapsed_.empty()) {
    std::set<int64_t> o;
    for (const int64_t i : collapsed_) o.emplace_hint(o.end(), i >= at ? i + count : i);
    collapsed_ = std::move(o);
  }
  dirty_ = true;
}

void DimIndex::Remove(int64_t at, int64_t count) {
  std::vector<int64_t> ni;
  std::vector<int32_t> ns, nsv;
  for (size_t j = 0; j < idx_.size(); ++j) {
    if ((idx_[j] >= at && idx_[j] < at + count) || Stale(j)) continue;  // the stale ones go too
    ni.push_back(idx_[j] >= at + count ? idx_[j] - count : idx_[j]);
    ns.push_back(size_[j]);
    nsv.push_back(saved_[j]);
  }
  idx_.swap(ni);
  size_.swap(ns);
  saved_.swap(nsv);
  stale_ = 0;
  if (!levels_.empty()) {
    std::map<int64_t, uint8_t> o;
    for (const auto& [i, l] : levels_) {
      if (i >= at && i < at + count) continue;
      o.emplace_hint(o.end(), i >= at + count ? i - count : i, l);  // in order: no search
    }
    levels_ = std::move(o);
    CountLevels();
  }
  if (!collapsed_.empty()) {
    std::set<int64_t> o;
    for (const int64_t i : collapsed_) {
      if (i < at || i >= at + count) o.emplace_hint(o.end(), i >= at + count ? i - count : i);
    }
    collapsed_ = std::move(o);
  }
  dirty_ = true;
}

std::vector<DimIndex::Entry> DimIndex::Entries() const {
  std::vector<Entry> out;
  out.reserve(idx_.size() - stale_);
  for (size_t j = 0; j < idx_.size(); ++j) {
    if (!Stale(j)) out.push_back({idx_[j], size_[j], saved_[j]});
  }
  return out;
}

void DimIndex::CopyRange(const DimIndex& from, int64_t a, int64_t b) {
  std::vector<Entry> out;
  for (const Entry& e : Entries()) {
    if (e.index < a || e.index > b) out.push_back(e);
  }
  for (const Entry& e : from.Entries()) {
    if (e.index >= a && e.index <= b) out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const Entry& x, const Entry& y) { return x.index < y.index; });
  Load(default_, out);
}

void DimIndex::Load(int32_t default_size, const std::vector<Entry>& entries) {
  default_ = default_size > 0 ? default_size : 1;
  idx_.clear();
  size_.clear();
  saved_.clear();
  stale_ = 0;
  for (const Entry& e : entries) {
    idx_.push_back(e.index);
    size_.push_back(e.size);
    saved_.push_back(e.saved > 0 ? e.saved : 0);
  }
  dirty_ = true;
}

}  // namespace yc::model
