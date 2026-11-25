// SPDX-License-Identifier: Apache-2.0
// tile_iter.h - visit only existing tiles and their cells inside a rectangle (cost proportional to the data, independent of the rectangle size)
#pragma once

#include "core/geometry.h"
#include "model/sheet.h"
#include "model/tile_dir.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <vector>

namespace yc::engine {

// fn(row_tile, col_tile, const T&)
template <class T, class Fn>
void ForEachTileInRect(const model::TileDir<T>& dir, const core::Rect& r, Fn&& fn) {
  const uint32_t cap = dir.RowTileCapacity();
  if (cap == 0) return;
  const uint32_t rt0 = r.r0 / model::kTileRows;
  const uint32_t rt1 = std::min<uint32_t>(r.r1 / model::kTileRows, cap - 1);
  const uint32_t ct0 = r.c0 / model::kTileCols;
  const uint32_t ct1 = std::min<uint32_t>(std::min<uint32_t>(r.c1, core::kMaxCols - 1) / model::kTileCols, model::kPageTiles - 1);
  for (uint32_t rt = rt0; rt <= rt1; ++rt) {
    if (!dir.HasRowBand(rt)) continue;
    dir.ForEachInBand(rt, ct0, ct1, [&](uint32_t ct, const T& t) { fn(rt, ct, t); });
  }
}

// fn(row, col, const T&, idx): every cell of the tile inside the rectangle (with or without a value)
template <class T, class Fn>
void ForEachCellInTiles(const model::TileDir<T>& dir, const core::Rect& r, Fn&& fn) {
  ForEachTileInRect(dir, r, [&](uint32_t rt, uint32_t ct, const T& t) {
    const uint32_t br = rt * model::kTileRows, bc = ct * model::kTileCols;
    const uint32_t r0 = std::max(r.r0, br), r1 = std::min(r.r1, br + model::kTileRows - 1);
    const uint32_t c0 = std::max(r.c0, bc), c1 = std::min(std::min(r.c1, core::kMaxCols - 1), bc + model::kTileCols - 1);
    for (uint32_t c = c0; c <= c1; ++c) {
      for (uint32_t row = r0; row <= r1; ++row) fn(row, c, t, model::TileIndex(row, c));
    }
  });
}

// non-empty values in row-major order (TEXTJOIN, CONCAT etc. need the order); fn returns false to stop
inline void ForEachValueRowMajor(const model::Sheet& s, const core::Rect& r,
                                 const std::function<bool(uint32_t, uint32_t, const core::Value&)>& fn) {
  const uint32_t cap = s.values.RowTileCapacity();
  if (cap == 0) return;
  const uint32_t rt0 = r.r0 / model::kTileRows, rt1 = std::min<uint32_t>(r.r1 / model::kTileRows, cap - 1);
  const uint32_t ct0 = r.c0 / model::kTileCols;
  const uint32_t ct1 = std::min<uint32_t>(std::min<uint32_t>(r.c1, core::kMaxCols - 1) / model::kTileCols, model::kPageTiles - 1);
  struct T {
    uint32_t ct;
    const model::ValueTile* t;
  };
  std::vector<T> band;
  for (uint32_t rt = rt0; rt <= rt1; ++rt) {
    if (!s.values.HasRowBand(rt)) continue;
    band.clear();
    s.values.ForEachInBand(rt, ct0, ct1, [&](uint32_t ct, const model::ValueTile& t) {
      if (t.count) band.push_back(T{ct, &t});
    });
    if (band.empty()) continue;
    const uint32_t br = rt * model::kTileRows;
    const uint32_t r0 = std::max(r.r0, br), r1 = std::min(r.r1, br + model::kTileRows - 1);
    if (band.size() == 1 && r.c0 == r.c1) {
      // single-column fast path: scan the bitmap directly
      const auto& t = *band[0].t;
      const uint32_t c = r.c0;
      const uint32_t base = (c % model::kTileCols) * model::kTileRows;
      for (uint32_t row = r0; row <= r1; ++row) {
        const uint32_t idx = base + row % model::kTileRows;
        const uint64_t w = t.occ[idx >> 6];
        if (w == 0) {
          // skip whole empty 64-cell words
          row = std::min(r1, row + (63 - (idx & 63)));
          continue;
        }
        if ((w >> (idx & 63)) & 1) {
          if (!fn(row, c, t.v[idx])) return;
        }
      }
      continue;
    }
    for (uint32_t row = r0; row <= r1; ++row) {
      for (const T& bt : band) {
        const uint32_t bc = bt.ct * model::kTileCols;
        const uint32_t c0 = std::max(r.c0, bc), c1 = std::min(std::min(r.c1, core::kMaxCols - 1), bc + model::kTileCols - 1);
        for (uint32_t c = c0; c <= c1; ++c) {
          const uint32_t idx = model::TileIndex(row, c);
          if (bt.t->Occupied(idx) && !fn(row, c, bt.t->v[idx])) return;
        }
      }
    }
  }
}

}  // namespace yc::engine
