// SPDX-License-Identifier: Apache-2.0
// dep_index.h - reverse dependency index (PLAN §7.2 RangeIndex)
//
// "Which formula cells reference this cell / range?" Buckets by the shape of the referenced rectangle:
//   small rectangles (<= 16 tiles)          -> one bucket per overlapping tile
//   tall rectangles (columns, A1:A100000)   -> one bucket per column tile band
//   wide rectangles (entire rows)           -> one bucket per row tile band
//   others (A:XFD and the like)             -> a sheet-level list
// Deletion is lazy: every formula cell has a generation number recorded at registration; re-registering or removing bumps it,
// queries skip old generations; compaction runs when stale entries pile up. So rewriting "100k cells of =SUM(A:A)" never scans big buckets.
#pragma once

#include "core/geometry.h"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yc::engine {

// cells changed in one propagation round, grouped by tile (256 rows x 16 columns): queries are batched per changed tile
// (PLAN §7.1), so a bucket is scanned once per round instead of once per changed cell
class ChangedCells {
 public:
  void Add(uint32_t sheet, uint32_t row, uint32_t col);
  void Finish();  // call after the last Add, before AnyIn
  bool empty() const { return tiles_.empty(); }
  // whether a changed cell lies inside rect on sheet
  bool AnyIn(uint32_t sheet, const core::Rect& rect) const;
  // a cell found to change while the set is queried: its bit is set when its tile is one of the set's (true), so the
  // rest of the query sees it; otherwise it waits for the next round (false)
  bool Mark(uint32_t sheet, uint32_t row, uint32_t col);

 private:
  friend class DepIndex;
  struct Tile {
    uint32_t sheet, rt, ct;
    std::bitset<4096> bits;  // column-major inside the tile: (col % 16) * 256 + row % 256
  };
  static uint64_t TileKey(uint32_t sheet, uint32_t rt, uint32_t ct) { return (uint64_t{sheet} << 48) | (uint64_t{rt} << 16) | ct; }
  static uint64_t BandKey(uint32_t sheet, uint32_t t) { return (uint64_t{sheet} << 32) | t; }
  bool TileHit(const Tile& t, const core::Rect& rect) const;

  std::vector<Tile> tiles_;
  std::unordered_map<uint64_t, size_t> index_;  // tile key -> position in tiles_
  // per column band (sheet, ct): (rt, position) sorted by rt, for tall rectangles
  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, size_t>>> bands_;
};

class DepIndex {
 public:
  // cell = core::CellKey(sheet_id, row, col) of the formula cell
  void Add(uint64_t cell, uint32_t ref_sheet, const core::Rect& rect);
  void Remove(uint64_t cell);  // remove every registration of this cell
  // query: fn(dependent cell key); a cell may be reported more than once (the caller dedupes)
  template <class Fn>
  void Query(uint32_t sheet, uint32_t row, uint32_t col, Fn&& fn) const;
  template <class Fn>
  void QueryRect(uint32_t sheet, const core::Rect& r, Fn&& fn) const;
  // every formula referencing at least one cell of `changed` (a cell may be reported more than once); fn(cell) returns
  // whether it marked a cell of `changed` (ChangedCells::Mark), which the entries scanned after it then see
  template <class Fn>
  void QueryChanged(ChangedCells& changed, Fn&& fn) const;

  void Clear();
  void Reserve(size_t cells) {  // before registering this many formulas at once (a rebuild)
    reg_.reserve(cells);
  }
  size_t live() const { return live_; }
  size_t stale() const { return stale_; }

 private:
  struct Entry {
    uint64_t cell;
    uint32_t gen;
    core::Rect rect;
  };
  using Bucket = std::vector<Entry>;

  static uint64_t TileKey(uint32_t sheet, uint32_t rt, uint32_t ct) { return (uint64_t{sheet} << 48) | (uint64_t{rt} << 16) | ct; }
  static uint64_t BandKey(uint32_t sheet, uint32_t t) { return (uint64_t{sheet} << 32) | t; }

  bool Valid(const Entry& e) const {
    const Reg* it = reg_.find(e.cell);
    return it && it->gen == e.gen;
  }
  void MaybeCompact();
  static void CompactBucket(Bucket& b, const DepIndex& self);

  std::unordered_map<uint64_t, Bucket> tiles_;
  // the tile bucket of the last registration: the formulas down a column read the same tile one after the other (a
  // node's address survives rehashing; reset where buckets are erased)
  uint64_t last_tile_ = ~0ull;
  Bucket* last_bucket_ = nullptr;
  std::unordered_map<uint64_t, Bucket> col_bands_;
  std::unordered_map<uint64_t, Bucket> row_bands_;
  std::unordered_map<uint32_t, Bucket> sheets_;
  struct Reg {
    uint32_t gen = 0;  // current generation
    uint32_t n = 0;    // registrations in the current generation
  };
  // cell -> Reg, open addressing in two flat arrays: a node allocated for each of millions of formula cells was most
  // of a rebuild. Cell keys never have all bits set (a column is below 2^14), so that marks an empty slot.
  class RegMap {
   public:
    static constexpr uint64_t kEmpty = ~0ull;
    Reg* find(uint64_t k) {
      if (keys_.empty()) return nullptr;
      for (size_t i = Slot(k);; i = (i + 1) & mask_) {
        if (keys_[i] == k) return &vals_[i];
        if (keys_[i] == kEmpty) return nullptr;
      }
    }
    const Reg* find(uint64_t k) const { return const_cast<RegMap*>(this)->find(k); }
    Reg& operator[](uint64_t k) {
      if ((size_ + 1) * 10 > keys_.size() * 7) Grow(std::max<size_t>(64, keys_.size() * 2));
      for (size_t i = Slot(k);; i = (i + 1) & mask_) {
        if (keys_[i] == k) return vals_[i];
        if (keys_[i] == kEmpty) {
          keys_[i] = k;
          vals_[i] = Reg{};
          ++size_;
          return vals_[i];
        }
      }
    }
    void reserve(size_t n) {
      size_t cap = 64;
      while (cap * 7 < n * 10) cap *= 2;
      if (cap > keys_.size()) Grow(cap);
    }
    template <class Pred>
    void erase_if(Pred&& drop) {  // rebuilt without them (no tombstones in a linear-probing table)
      RegMap out;
      out.reserve(size_);
      for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] != kEmpty && !drop(vals_[i])) out[keys_[i]] = vals_[i];
      }
      swap(out);
    }
    void swap(RegMap& o) noexcept {
      keys_.swap(o.keys_);
      vals_.swap(o.vals_);
      std::swap(size_, o.size_);
      std::swap(mask_, o.mask_);
    }
    size_t size() const { return size_; }

   private:
    size_t Slot(uint64_t k) const { return static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> 20) & mask_; }
    void Grow(size_t cap) {
      std::vector<uint64_t> ok;
      std::vector<Reg> ov;
      ok.swap(keys_);
      ov.swap(vals_);
      keys_.assign(cap, kEmpty);
      vals_.resize(cap);
      mask_ = cap - 1;
      size_ = 0;
      for (size_t i = 0; i < ok.size(); ++i) {
        if (ok[i] != kEmpty) (*this)[ok[i]] = ov[i];
      }
    }
    std::vector<uint64_t> keys_;
    std::vector<Reg> vals_;
    size_t size_ = 0, mask_ = 0;
  };
  RegMap reg_;
  size_t live_ = 0, stale_ = 0;
};

// ---- inline ----

template <class Fn>
void DepIndex::Query(uint32_t sheet, uint32_t row, uint32_t col, Fn&& fn) const {
  auto scan = [&](const Bucket& b) {
    for (const Entry& e : b) {
      if (e.rect.Contains(row, col) && Valid(e)) fn(e.cell);
    }
  };
  const uint32_t rt = row / 256, ct = col / 16;
  if (auto it = tiles_.find(TileKey(sheet, rt, ct)); it != tiles_.end()) scan(it->second);
  if (auto it = col_bands_.find(BandKey(sheet, ct)); it != col_bands_.end()) scan(it->second);
  if (auto it = row_bands_.find(BandKey(sheet, rt)); it != row_bands_.end()) scan(it->second);
  if (auto it = sheets_.find(sheet); it != sheets_.end()) scan(it->second);
}

template <class Fn>
void DepIndex::QueryChanged(ChangedCells& ch, Fn&& fn) const {
  // the formulas down a column read the same range one after the other (a calculated column of =COUNT(A:A) over a
  // table as tall as the sheet: a million entries of A:A): the answer for a range is asked once for its run, not once
  // an entry (each walked the column's tiles, 10^10 steps: a step that never ended, doc_fuzz --wide seed 23400083)
  auto scan = [&](uint32_t sheet, const Bucket& b) {
    const core::Rect* last = nullptr;
    bool last_hit = false;
    for (const Entry& e : b) {
      if (!last || !(*last == e.rect)) {
        last = &e.rect;
        last_hit = ch.AnyIn(sheet, e.rect);
      }
      if (last_hit && Valid(e) && fn(e.cell)) last = nullptr;  // a cell marked: the answers asked before may change
    }
  };
  std::unordered_set<uint64_t> col_seen, row_seen;
  std::unordered_set<uint32_t> sheet_seen;
  for (const auto& t : ch.tiles_) {
    if (auto it = tiles_.find(TileKey(t.sheet, t.rt, t.ct)); it != tiles_.end()) scan(t.sheet, it->second);
    if (col_seen.insert(BandKey(t.sheet, t.ct)).second) {
      if (auto it = col_bands_.find(BandKey(t.sheet, t.ct)); it != col_bands_.end()) scan(t.sheet, it->second);
    }
    if (row_seen.insert(BandKey(t.sheet, t.rt)).second) {
      if (auto it = row_bands_.find(BandKey(t.sheet, t.rt)); it != row_bands_.end()) scan(t.sheet, it->second);
    }
    if (sheet_seen.insert(t.sheet).second) {
      if (auto it = sheets_.find(t.sheet); it != sheets_.end()) scan(t.sheet, it->second);
    }
  }
}

template <class Fn>
void DepIndex::QueryRect(uint32_t sheet, const core::Rect& r, Fn&& fn) const {
  auto scan = [&](const Bucket& b) {
    for (const Entry& e : b) {
      if (e.rect.Intersects(r) && Valid(e)) fn(e.cell);
    }
  };
  const uint32_t rt0 = r.r0 / 256, rt1 = r.r1 / 256, ct0 = r.c0 / 16, ct1 = std::min<uint32_t>(r.c1, core::kMaxCols - 1) / 16;
  const uint64_t ntiles = (uint64_t{rt1} - rt0 + 1) * (uint64_t{ct1} - ct0 + 1);
  if (ntiles <= tiles_.size()) {
    for (uint32_t rt = rt0; rt <= rt1; ++rt) {
      for (uint32_t ct = ct0; ct <= ct1; ++ct) {
        if (auto it = tiles_.find(TileKey(sheet, rt, ct)); it != tiles_.end()) scan(it->second);
      }
      if (rt == 0xFFFFFFFFu) break;
    }
  } else {
    for (const auto& [k, b] : tiles_) {
      if ((k >> 48) == sheet) scan(b);
    }
  }
  for (uint32_t ct = ct0; ct <= ct1; ++ct) {
    if (auto it = col_bands_.find(BandKey(sheet, ct)); it != col_bands_.end()) scan(it->second);
  }
  if (uint64_t{rt1} - rt0 + 1 <= row_bands_.size()) {
    for (uint32_t rt = rt0; rt <= rt1; ++rt) {
      if (auto it = row_bands_.find(BandKey(sheet, rt)); it != row_bands_.end()) scan(it->second);
      if (rt == 0xFFFFFFFFu) break;
    }
  } else {
    for (const auto& [k, b] : row_bands_) {
      if ((k >> 32) == sheet) scan(b);
    }
  }
  if (auto it = sheets_.find(sheet); it != sheets_.end()) scan(it->second);
}

}  // namespace yc::engine
