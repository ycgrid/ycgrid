// SPDX-License-Identifier: Apache-2.0
// dim_index.h - sparse row-height / column-width index (PLAN §5.7)
//
// "Default size + only custom sizes": memory grows with the number of custom entries k, not with the number of rows.
//   Offset(i) = i x default + sum(deltas of custom entries with index < i)   O(log k)
//   IndexAt(px): which entry a pixel falls into                            O(log k)
// Units: pixels at 100% zoom (int64). Hidden = size 0; the original size is kept for unhiding.
// Implementation: sorted array + prefix deltas, rebuilt lazily after writes (once per batch of writes). An entry set
// back to the default stays in place as a stale one and is compacted away later: erasing it at once made unhiding or
// resetting all 1,048,576 rows O(n^2) (YCGrid froze for minutes on Ctrl+Shift+9 with whole columns selected).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace yc::model {

class DimIndex {
 public:
  explicit DimIndex(int32_t default_size = 20) : default_(default_size) {}

  int32_t default_size() const { return default_; }
  void set_default_size(int32_t s) {
    if (stale_) Compact();  // a stale entry holds the old default
    default_ = s > 0 ? s : 1;  // 0 (a damaged file, defaultRowHeight="0") would divide by zero
    dirty_ = true;
  }

  int32_t Size(int64_t index) const;
  bool IsCustom(int64_t index) const;
  bool IsHidden(int64_t index) const { return Size(index) == 0; }
  void Set(int64_t index, int32_t size);       // the custom entry is removed when size == default
  void Hide(int64_t index, bool hidden);       // keep the original size
  void Reset(int64_t index) { Set(index, default_); }

  int64_t Offset(int64_t index) const;
  int64_t IndexAt(int64_t px) const;
  int64_t NextVisible(int64_t index, int dir, int64_t limit) const;
  size_t custom_count() const { return idx_.size() - stale_; }
  // call before publishing a snapshot: finish the lazy rebuild so mutable members are never written while threads read the snapshot
  void Finalize() const {
    if (dirty_) Rebuild();
  }

  // outline (Data -> Group): level 0..7 per row / column, only non-zero levels are stored
  static constexpr int kMaxOutline = 7;
  int Level(int64_t index) const {
    auto it = levels_.find(index);
    return it == levels_.end() ? 0 : it->second;
  }
  void SetLevel(int64_t index, int level);
  // the deepest level (O(1): the rows / columns at each level are counted as levels change; every frame asked, and a
  // million rows grouped by Subtotal took a frame's time to scan)
  int MaxLevel() const;
  const std::map<int64_t, uint8_t>& Levels() const { return levels_; }

  // offset custom entries when rows/columns are inserted or deleted
  void Insert(int64_t at, int64_t count);
  void Remove(int64_t at, int64_t count);

  // for serialization
  struct Entry {
    int64_t index;
    int32_t size;
    int32_t saved;  // size before hiding (0 = none)
  };
  std::vector<Entry> Entries() const;
  void Load(int32_t default_size, const std::vector<Entry>& entries);
  // the sizes and hidden states of [a, b] as they are in another version (undo of a row height / column width change)
  void CopyRange(const DimIndex& from, int64_t a, int64_t b);
  void LoadLevels(std::map<int64_t, uint8_t> levels) {
    levels_ = std::move(levels);
    CountLevels();
  }
  // the summary lines of the groups a person collapsed (Excel's collapsed flag, kept in its files too): showing an
  // outer group's detail leaves these groups collapsed and shows the others whole; they move with inserted and
  // deleted lines
  bool IsCollapsed(int64_t index) const { return collapsed_.count(index) != 0; }
  void SetCollapsed(int64_t index, bool on) {
    if (on) collapsed_.insert(index);
    else collapsed_.erase(index);
  }
  const std::set<int64_t>& Collapsed() const { return collapsed_; }
  void LoadCollapsed(std::set<int64_t> c) { collapsed_ = std::move(c); }

 private:
  void Rebuild() const;
  size_t Find(int64_t index) const;  // lower_bound
  bool Stale(size_t j) const { return saved_[j] < 0; }  // set back to the default: saved_ -1, size_ the default
  void Compact();  // drops the stale entries

  int32_t default_;
  std::vector<int64_t> idx_;
  std::vector<int32_t> size_;
  std::vector<int32_t> saved_;
  std::map<int64_t, uint8_t> levels_;
  std::set<int64_t> collapsed_;
  void CountLevels();
  static int Slot(int level) { return level < 0 ? 0 : level > kMaxOutline ? kMaxOutline : level; }  // a file's 9 counts as 7
  std::array<int64_t, kMaxOutline + 1> level_count_{};  // entries at each level (slot 0: entries stored as 0)
  mutable std::vector<int64_t> before_;
  mutable bool dirty_ = false;
  size_t stale_ = 0;  // entries back at the default size, not yet dropped
};

}  // namespace yc::model
