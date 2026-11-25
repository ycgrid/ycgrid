// SPDX-License-Identifier: Apache-2.0
#include "model/sheet.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace yc::model {

namespace {

struct TilePool {
  std::mutex mu;
  // the freed blocks of one size; min_count is the fewest it held at any time in this window: that many blocks (the
  // oldest, at the front) weren't needed all window and go back to Windows when it ends, so memory returns to what
  // the steady load needs after a burst. Each size counts for itself: a shared count took the block another size
  // had just freed, which its next allocation wanted.
  struct FreeList {
    size_t size = 0;
    std::vector<void*> blocks;
    size_t min_count = 0;
  };
  std::vector<FreeList> free;  // only a few sizes
  static constexpr size_t kKeepBytes = size_t{96} << 20;  // freed blocks kept, all sizes together
  size_t kept = 0;
  std::chrono::steady_clock::time_point window_start{};
};

TilePool& Pool() {
  static TilePool* p = new TilePool();  // never destroyed: tiles may be freed during static destruction
  return *p;
}

constexpr std::align_val_t kTileAlign{64};

// with p.mu held: when the window is over, takes out of each list the blocks that were idle all of it (freed by the
// caller, outside the lock)
void TrimLocked(TilePool& p, std::vector<std::pair<void*, size_t>>* out, bool now_ = false) {
  for (auto& l : p.free) l.min_count = std::min(l.min_count, l.blocks.size());
  const auto now = std::chrono::steady_clock::now();
  if (!now_ && now - p.window_start < std::chrono::seconds(10)) return;
  for (auto& l : p.free) {
    // the oldest blocks are at the front; the back holds the ones just freed, which the next allocation takes
    const size_t n = std::min(l.min_count, l.blocks.size());
    for (size_t i = 0; i < n; ++i) {
      out->emplace_back(l.blocks[i], l.size);
      p.kept -= l.size;
    }
    l.blocks.erase(l.blocks.begin(), l.blocks.begin() + static_cast<std::ptrdiff_t>(n));
    l.min_count = l.blocks.size();
  }
  p.window_start = now;
}

void FreeBlocks(const std::vector<std::pair<void*, size_t>>& blocks) {
  for (const auto& b : blocks) ::operator delete(b.first, kTileAlign);
}

}  // namespace

size_t TileMemKept() {
  TilePool& p = Pool();
  std::lock_guard<std::mutex> lk(p.mu);
  return p.kept;
}

void TileMemTrimIdle() {
  std::vector<std::pair<void*, size_t>> out;
  {
    TilePool& p = Pool();
    std::lock_guard<std::mutex> lk(p.mu);
    TrimLocked(p, &out, true);
  }
  FreeBlocks(out);
}

void* TileMemAlloc(size_t bytes) {
  TilePool& p = Pool();
  void* m = nullptr;
  std::vector<std::pair<void*, size_t>> trimmed;
  {
    std::lock_guard<std::mutex> lk(p.mu);
    for (auto& l : p.free) {
      if (l.size == bytes && !l.blocks.empty()) {
        m = l.blocks.back();
        l.blocks.pop_back();
        p.kept -= bytes;
        break;
      }
    }
    TrimLocked(p, &trimmed);
  }
  FreeBlocks(trimmed);
  return m ? m : ::operator new(bytes, kTileAlign);
}

void TileMemFree(void* m, size_t bytes) noexcept {
  TilePool& p = Pool();
  bool kept = false;
  std::vector<std::pair<void*, size_t>> trimmed;
  try {
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.kept + bytes <= TilePool::kKeepBytes) {
      for (auto& l : p.free) {
        if (l.size == bytes) {
          l.blocks.push_back(m);
          kept = true;
          break;
        }
      }
      if (!kept) {
        p.free.push_back(TilePool::FreeList{bytes, std::vector<void*>{m}, 0});  // held none this window so far
        kept = true;
      }
      p.kept += bytes;
    }
    TrimLocked(p, &trimmed);
  } catch (...) {  // out of memory for the list: the block goes back to Windows
  }
  FreeBlocks(trimmed);
  if (!kept) ::operator delete(m, kTileAlign);
}

void Sheet::ComputeUsedRange() {
  has_used = false;
  auto add = [this](uint32_t r, uint32_t c) {
    if (!has_used) {
      used = core::Rect::Cell(r, c);
      has_used = true;
    } else {
      used = used.Union(core::Rect::Cell(r, c));
    }
  };
  values.ForEach([&](uint32_t rt, uint32_t ct, const ValueTile& t) {
    if (t.count == 0) return;
    for (uint32_t w = 0; w < kTileCells / 64; ++w) {
      uint64_t bits = t.occ[w];
      while (bits) {
        const uint32_t b = static_cast<uint32_t>(std::countr_zero(bits));
        bits &= bits - 1;
        const uint32_t idx = w * 64 + b;
        add(rt * kTileRows + idx % kTileRows, ct * kTileCols + idx / kTileRows);
      }
    }
  });
  meta.ForEach([&](uint32_t rt, uint32_t ct, const MetaTile& t) {
    if (t.formula_count == 0 && t.style_count == 0) return;
    for (uint32_t idx = 0; idx < kTileCells; ++idx) {
      if (t.formula[idx] || t.style[idx]) add(rt * kTileRows + idx % kTileRows, ct * kTileCols + idx / kTileRows);
    }
  });
  for (const auto& m : *merges) {
    add(m.r0, m.c0);
    add(m.r1, m.c1);
  }
}

}  // namespace yc::model
