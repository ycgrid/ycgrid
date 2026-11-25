// SPDX-License-Identifier: Apache-2.0
// tile_dir.h - three-level, allocate-on-demand, copy-on-write tile directory (PLAN §5.3)
//
//   Dir: vector<shared_ptr<Chunk>>, every Chunk covers 256 row tiles (65,536 rows)
//   Chunk: 256 shared_ptr<Page> (one row-tile band each)
//   Page: 1,024 shared_ptr<T> (column tiles, 16,384 columns / 16)
// A tile is 256 rows x 16 columns. When RowLimit grows the Dir just gets longer; old snapshots are unaffected.
// Publishing a snapshot copies the Dir (a vector of Chunk pointers): about 1,500 pointers even for 100 million rows.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace yc::model {

inline constexpr uint32_t kTileRows = 256;
inline constexpr uint32_t kTileCols = 16;
inline constexpr uint32_t kTileCells = kTileRows * kTileCols;
inline constexpr uint32_t kPageTiles = 1024;   // column tiles per row-tile band
inline constexpr uint32_t kChunkPages = 256;   // row-tile bands per Chunk

inline uint32_t TileIndex(uint32_t row, uint32_t col) { return (col % kTileCols) * kTileRows + (row % kTileRows); }

// Memory for large tiles (64 KB values, 32 KB styles and formulas) comes from a pool: live data copies hundreds of
// tiles a second (copy on write after each publish), and the system heap gives blocks this size back to Windows when
// they are freed, so every new one would page-fault again. The pool keeps a bounded number of freed blocks per size.
void* TileMemAlloc(size_t bytes);
void TileMemFree(void* p, size_t bytes) noexcept;
size_t TileMemKept();     // bytes of freed blocks the pool holds
void TileMemTrimIdle();   // ends the idle window now (tests): blocks idle since the last call go back to Windows

template <class U>
struct TileAllocator {
  using value_type = U;
  TileAllocator() = default;
  template <class V>
  TileAllocator(const TileAllocator<V>&) noexcept {}
  U* allocate(size_t n) { return static_cast<U*>(TileMemAlloc(n * sizeof(U))); }
  void deallocate(U* p, size_t n) noexcept { TileMemFree(p, n * sizeof(U)); }
  template <class V>
  bool operator==(const TileAllocator<V>&) const noexcept {
    return true;
  }
};

template <class T, class... Args>
std::shared_ptr<T> NewTile(Args&&... args) {
  if constexpr (sizeof(T) >= 16384) return std::allocate_shared<T>(TileAllocator<T>{}, std::forward<Args>(args)...);
  else return std::make_shared<T>(std::forward<Args>(args)...);
}

template <class T>
class TileDir {
 public:
  struct Page {
    std::array<std::shared_ptr<T>, kPageTiles> tiles{};
  };
  struct Chunk {
    std::array<std::shared_ptr<Page>, kChunkPages> pages{};
  };

  // ---- Reading (snapshots and the working version) ----
  const T* Get(uint32_t row_tile, uint32_t col_tile) const {
    const uint32_t ci = row_tile / kChunkPages;
    if (ci >= chunks_.size() || !chunks_[ci]) return nullptr;
    const auto& page = chunks_[ci]->pages[row_tile % kChunkPages];
    if (!page || col_tile >= kPageTiles) return nullptr;
    return page->tiles[col_tile].get();
  }
  const T* GetCell(uint32_t row, uint32_t col) const { return Get(row / kTileRows, col / kTileCols); }
  std::shared_ptr<const T> Share(uint32_t row_tile, uint32_t col_tile) const {
    const uint32_t ci = row_tile / kChunkPages;
    if (ci >= chunks_.size() || !chunks_[ci]) return nullptr;
    const auto& page = chunks_[ci]->pages[row_tile % kChunkPages];
    if (!page || col_tile >= kPageTiles) return nullptr;
    return page->tiles[col_tile];
  }
  bool HasRowBand(uint32_t row_tile) const {
    const uint32_t ci = row_tile / kChunkPages;
    return ci < chunks_.size() && chunks_[ci] && chunks_[ci]->pages[row_tile % kChunkPages];
  }
  uint32_t RowTileCapacity() const { return static_cast<uint32_t>(chunks_.size()) * kChunkPages; }

  // ---- Writing (working version only, engine thread) ----
  T& Mutable(uint32_t row_tile, uint32_t col_tile) {
    assert(col_tile < kPageTiles);  // callers check the column against kMaxCols; never write past the page
    if (col_tile >= kPageTiles) col_tile = kPageTiles - 1;
    const uint32_t ci = row_tile / kChunkPages;
    if (ci >= chunks_.size()) chunks_.resize(ci + 1);
    auto& chunk = chunks_[ci];
    if (!chunk) chunk = std::make_shared<Chunk>();
    else if (chunk.use_count() > 1) chunk = std::make_shared<Chunk>(*chunk);
    auto& page = chunk->pages[row_tile % kChunkPages];
    if (!page) page = std::make_shared<Page>();
    else if (page.use_count() > 1) page = std::make_shared<Page>(*page);
    auto& tile = page->tiles[col_tile];
    if (!tile) tile = NewTile<T>();
    else if (tile.use_count() > 1) tile = NewTile<T>(*tile);
    return *tile;
  }
  T& MutableCell(uint32_t row, uint32_t col) { return Mutable(row / kTileRows, col / kTileCols); }

  // replace one tile directly (undo puts the old tile back)
  void Replace(uint32_t row_tile, uint32_t col_tile, std::shared_ptr<T> t) {
    const uint32_t ci = row_tile / kChunkPages;
    if (ci >= chunks_.size()) chunks_.resize(ci + 1);
    auto& chunk = chunks_[ci];
    if (!chunk) chunk = std::make_shared<Chunk>();
    else if (chunk.use_count() > 1) chunk = std::make_shared<Chunk>(*chunk);
    auto& page = chunk->pages[row_tile % kChunkPages];
    if (!page) page = std::make_shared<Page>();
    else if (page.use_count() > 1) page = std::make_shared<Page>(*page);
    page->tiles[col_tile] = std::move(t);
  }

  // visit every existing tile: fn(row_tile, col_tile, const T&)
  template <class Fn>
  void ForEach(Fn&& fn) const {
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
      if (!chunks_[ci]) continue;
      for (uint32_t pi = 0; pi < kChunkPages; ++pi) {
        const auto& page = chunks_[ci]->pages[pi];
        if (!page) continue;
        for (uint32_t ct = 0; ct < kPageTiles; ++ct) {
          if (page->tiles[ct]) fn(ci * kChunkPages + pi, ct, *page->tiles[ct]);
        }
      }
    }
  }
  // only visit the tiles of one row-tile band with column tiles in [ct0, ct1]
  template <class Fn>
  void ForEachInBand(uint32_t row_tile, uint32_t ct0, uint32_t ct1, Fn&& fn) const {
    const uint32_t ci = row_tile / kChunkPages;
    if (ci >= chunks_.size() || !chunks_[ci]) return;
    const auto& page = chunks_[ci]->pages[row_tile % kChunkPages];
    if (!page) return;
    for (uint32_t ct = ct0; ct <= ct1 && ct < kPageTiles; ++ct) {
      if (page->tiles[ct]) fn(ct, *page->tiles[ct]);
    }
  }

  void Clear() { chunks_.clear(); }

 private:
  std::vector<std::shared_ptr<Chunk>> chunks_;
};

}  // namespace yc::model
