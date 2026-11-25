// SPDX-License-Identifier: Apache-2.0
#include "engine/dep_index.h"

#include <memory>
#include <thread>

#include <algorithm>

namespace yc::engine {

// ---- ChangedCells ----

void ChangedCells::Add(uint32_t sheet, uint32_t row, uint32_t col) {
  const uint32_t rt = row / 256, ct = col / 16;
  const uint64_t k = TileKey(sheet, rt, ct);
  auto it = index_.find(k);
  size_t pos;
  if (it == index_.end()) {
    pos = tiles_.size();
    tiles_.push_back(Tile{sheet, rt, ct, {}});
    index_.emplace(k, pos);
  } else {
    pos = it->second;
  }
  tiles_[pos].bits.set((col % 16) * 256 + row % 256);
}

void ChangedCells::Finish() {
  bands_.clear();
  for (size_t i = 0; i < tiles_.size(); ++i) bands_[BandKey(tiles_[i].sheet, tiles_[i].ct)].emplace_back(tiles_[i].rt, i);
  for (auto& [k, v] : bands_) std::sort(v.begin(), v.end());
}

bool ChangedCells::TileHit(const Tile& t, const core::Rect& rect) const {
  const uint32_t tr0 = t.rt * 256, tc0 = t.ct * 16;
  const uint32_t r0 = std::max(rect.r0, tr0), r1 = std::min(rect.r1, tr0 + 255);
  const uint32_t c0 = std::max(rect.c0, tc0), c1 = std::min(rect.c1, tc0 + 15);
  if (r0 > r1 || c0 > c1) return false;
  if (r0 == tr0 && r1 == tr0 + 255 && c0 == tc0 && c1 == tc0 + 15) return t.bits.any();  // the whole tile
  for (uint32_t c = c0; c <= c1; ++c) {
    const size_t base = (c - tc0) * 256;
    for (uint32_t r = r0; r <= r1; ++r) {
      if (t.bits.test(base + (r - tr0))) return true;
    }
  }
  return false;
}

bool ChangedCells::Mark(uint32_t sheet, uint32_t row, uint32_t col) {
  auto it = index_.find(TileKey(sheet, row / 256, col / 16));
  if (it == index_.end()) return false;
  tiles_[it->second].bits.set((col % 16) * 256 + row % 256);
  return true;
}

bool ChangedCells::AnyIn(uint32_t sheet, const core::Rect& rect_in) const {
  core::Rect rect = rect_in;
  rect.c1 = std::min<uint32_t>(rect.c1, core::kMaxCols - 1);
  if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return false;
  const uint32_t rt0 = rect.r0 / 256, rt1 = rect.r1 / 256, ct0 = rect.c0 / 16, ct1 = rect.c1 / 16;
  if (rt0 == rt1 && ct0 == ct1) {  // the common case: inside one tile
    auto it = index_.find(TileKey(sheet, rt0, ct0));
    return it != index_.end() && TileHit(tiles_[it->second], rect);
  }
  // otherwise walk the changed tiles of each column band the rectangle covers
  for (uint32_t ct = ct0; ct <= ct1; ++ct) {
    auto it = bands_.find(BandKey(sheet, ct));
    if (it == bands_.end()) continue;
    const auto& v = it->second;
    for (auto p = std::lower_bound(v.begin(), v.end(), std::make_pair(rt0, size_t{0})); p != v.end() && p->first <= rt1; ++p) {
      if (TileHit(tiles_[p->second], rect)) return true;
    }
  }
  return false;
}

// ---- DepIndex ----

void DepIndex::Add(uint64_t cell, uint32_t sheet, const core::Rect& rect_in) {
  core::Rect rect = rect_in;
  rect.c1 = std::min<uint32_t>(rect.c1, core::kMaxCols - 1);
  Reg& rg = reg_[cell];
  const uint32_t gen = rg.gen;
  ++rg.n;
  ++live_;
  const Entry e{cell, gen, rect};
  const uint32_t rt0 = rect.r0 / 256, rt1 = rect.r1 / 256, ct0 = rect.c0 / 16, ct1 = rect.c1 / 16;
  const uint64_t nr = uint64_t{rt1} - rt0 + 1, nc = uint64_t{ct1} - ct0 + 1;
  if (nr * nc <= 16) {
    for (uint32_t rt = rt0; rt <= rt1; ++rt) {
      for (uint32_t ct = ct0; ct <= ct1; ++ct) {
        const uint64_t tk = TileKey(sheet, rt, ct);
        if (tk != last_tile_ || !last_bucket_) {
          last_tile_ = tk;
          last_bucket_ = &tiles_[tk];
        }
        last_bucket_->push_back(e);
      }
    }
  } else if (nc <= 16) {
    for (uint32_t ct = ct0; ct <= ct1; ++ct) col_bands_[BandKey(sheet, ct)].push_back(e);
  } else if (nr <= 16) {
    for (uint32_t rt = rt0; rt <= rt1; ++rt) row_bands_[BandKey(sheet, rt)].push_back(e);
  } else {
    sheets_[sheet].push_back(e);
  }
}

void DepIndex::Remove(uint64_t cell) {
  Reg* it = reg_.find(cell);
  if (!it || it->n == 0) return;
  live_ -= it->n;
  stale_ += it->n;
  it->n = 0;
  ++it->gen;
  MaybeCompact();
}

void DepIndex::CompactBucket(Bucket& b, const DepIndex& self) {
  b.erase(std::remove_if(b.begin(), b.end(), [&](const Entry& e) { return !self.Valid(e); }), b.end());
}

void DepIndex::MaybeCompact() {
  if (stale_ < 4096 || stale_ < live_) return;
  last_bucket_ = nullptr;
  for (auto* m : {&tiles_, &col_bands_, &row_bands_}) {
    for (auto it = m->begin(); it != m->end();) {
      CompactBucket(it->second, *this);
      it = it->second.empty() ? m->erase(it) : std::next(it);
    }
  }
  for (auto it = sheets_.begin(); it != sheets_.end();) {
    CompactBucket(it->second, *this);
    it = it->second.empty() ? sheets_.erase(it) : std::next(it);
  }
  // cells without registrations no longer need a generation number
  reg_.erase_if([](const Reg& r) { return r.n == 0; });
  stale_ = 0;
}

void DepIndex::Clear() {
  // millions of entries are freed on a thread of their own: freeing them here held the engine for half a second
  // at every rebuild (inserting a row, undo...)
  struct Old {
    decltype(tiles_) a, b, c;
    decltype(sheets_) d;
    decltype(reg_) e;
  };
  auto old = std::make_unique<Old>();
  old->a.swap(tiles_);
  old->b.swap(col_bands_);
  old->c.swap(row_bands_);
  old->d.swap(sheets_);
  old->e.swap(reg_);
  if (old->e.size() > 100000) std::thread([o = std::move(old)]() mutable { o.reset(); }).detach();
  live_ = stale_ = 0;
  last_bucket_ = nullptr;
}

}  // namespace yc::engine
