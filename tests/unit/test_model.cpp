// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "engine/engine.h"
#include "model/dim_index.h"
#include "model/sheet.h"

#include <memory>
#include <random>
#include <vector>

using namespace yc::model;

TEST_CASE("DimIndex 稀疏列高與樸素陣列一致（屬性測試）") {
  DimIndex d(20);
  std::vector<int32_t> naive(5000, 20);
  std::mt19937 rng(42);
  for (int i = 0; i < 2000; ++i) {
    const int64_t idx = rng() % 5000;
    const int32_t sz = static_cast<int32_t>(rng() % 60);
    d.Set(idx, sz);
    naive[static_cast<size_t>(idx)] = sz;
    if (i % 97 == 0) {
      int64_t off = 0;
      for (int64_t k = 0; k < 5000; k += 37) {
        int64_t expect = 0;
        for (int64_t j = 0; j < k; ++j) expect += naive[static_cast<size_t>(j)];
        CHECK(d.Offset(k) == expect);
        (void)off;
      }
    }
  }
  // IndexAt lookup back
  int64_t acc = 0;
  for (int64_t k = 0; k < 5000; ++k) {
    if (naive[static_cast<size_t>(k)] > 0) CHECK(d.IndexAt(acc) == k);
    acc += naive[static_cast<size_t>(k)];
  }
  // 100 million rows: only the custom entries are stored
  DimIndex big(20);
  big.Set(99999999, 40);
  CHECK(big.Offset(100000000) == int64_t{99999999} * 20 + 40);
  CHECK(big.custom_count() == 1);
}

TEST_CASE("Sheet tile 讀寫與 COW 共用") {
  Sheet s;
  s.SetValue(5, 3, yc::core::Value::Number(7));
  s.SetValue(1000000, 16000, yc::core::Value::String("far"));
  CHECK(s.GetValue(5, 3).number() == 7);
  CHECK(s.GetValue(1000000, 16000).str() == "far");
  Sheet snap = s;  // snapshot
  s.SetValue(5, 3, yc::core::Value::Number(8));
  CHECK(snap.GetValue(5, 3).number() == 7);
  CHECK(s.GetValue(5, 3).number() == 8);
  s.ComputeUsedRange();
  CHECK(s.used.r1 == 1000000);
  CHECK(s.used.c1 == 16000);
}

namespace {

// The tile pool is shared by the whole process, and the engines' release thread frees tiles into it at any moment
// (undo entries earlier tests' Documents dropped): a test that checks which block comes back waits for that thread
// first, then empties the pool. Without it the check failed on a slow CI machine when a tile freed in between was
// the one handed out.
void QuietTilePool() {
  yc::engine::WaitForReleases();
  yc::model::TileMemTrimIdle();
  yc::model::TileMemTrimIdle();
}

}  // namespace

TEST_CASE("value tiles: copy on write keeps strings alive, and pooled tile memory is reused") {
  using yc::core::Value;
  // what an earlier test's Document may have left with the release thread
  for (int i = 0; i < 60; ++i) {
    auto big = std::make_shared<std::vector<std::shared_ptr<yc::model::ValueTile>>>();
    for (int k = 0; k < 16; ++k) big->push_back(yc::model::NewTile<yc::model::ValueTile>());
    yc::engine::ReleaseOnWorker(std::move(big));
  }
  QuietTilePool();
  auto a = yc::model::NewTile<yc::model::ValueTile>();
  const std::string longer = "a string longer than the inline limit";
  a->Set(0, Value::String(longer));
  a->Set(5, Value::Number(1.5));
  a->Set(4095, Value::String("short"));
  auto b = yc::model::NewTile<yc::model::ValueTile>(*a);
  CHECK(b->count == 3);
  a.reset();  // the original goes away: the copy's heap string must still be there
  CHECK(b->v[0].str() == longer);
  CHECK(b->v[5].AsDouble() == 1.5);
  CHECK(b->v[4095].str() == "short");
  CHECK(b->Occupied(4095));
  CHECK_FALSE(b->Occupied(1));
  // freed blocks come back from the pool
  const void* first = b.get();
  b.reset();
  auto c = yc::model::NewTile<yc::model::ValueTile>();
  CHECK(static_cast<const void*>(c.get()) == first);
  CHECK(c->count == 0);
  CHECK(c->v[0].IsEmpty());
  // blocks that stay unused for a whole window go back to Windows
  {
    auto d = yc::model::NewTile<yc::model::ValueTile>();
    auto e = yc::model::NewTile<yc::model::ValueTile>();
  }
  CHECK(yc::model::TileMemKept() > 0);
  yc::model::TileMemTrimIdle();  // a new window starts with these blocks idle
  yc::model::TileMemTrimIdle();  // they stayed idle all of it
  CHECK(yc::model::TileMemKept() == 0);
}

TEST_CASE("value tiles: the pool gives back the blocks idle all window, not the one just freed") {
  QuietTilePool();
  auto t1 = yc::model::NewTile<yc::model::ValueTile>();
  auto t2 = yc::model::NewTile<yc::model::ValueTile>();
  const void* cold = t1.get();
  const void* hot = t2.get();
  t1.reset();  // freed first: the oldest block in the pool
  t2.reset();  // freed last: the one the next allocation takes
  yc::model::TileMemTrimIdle();  // the window starts with both blocks in the pool
  {
    auto t = yc::model::NewTile<yc::model::ValueTile>();  // takes the last freed block and frees it again
    CHECK(static_cast<const void*>(t.get()) == hot);
  }
  yc::model::TileMemTrimIdle();  // one block was idle all window: the cold one goes, the hot one stays
  auto c = yc::model::NewTile<yc::model::ValueTile>();
  CHECK(static_cast<const void*>(c.get()) == hot);
  CHECK(static_cast<const void*>(c.get()) != cold);
  c.reset();
  yc::model::TileMemTrimIdle();
  yc::model::TileMemTrimIdle();
}

TEST_CASE("value tiles: each block size counts its own idle blocks: a big block idle all window doesn't cost the small one just freed") {
  static_assert(sizeof(yc::model::MetaTile) != sizeof(yc::model::ValueTile));  // two lists in the pool
  QuietTilePool();
  // the small size's list before the big one's (a list stays once made, empty or not)
  { auto m = yc::model::NewTile<yc::model::MetaTile>(); }
  { auto v = yc::model::NewTile<yc::model::ValueTile>(); }
  yc::model::TileMemTrimIdle();
  yc::model::TileMemTrimIdle();
  CHECK(yc::model::TileMemKept() == 0);
  auto v = yc::model::NewTile<yc::model::ValueTile>();
  v.reset();                     // a big block, idle for the whole window that starts now
  const size_t big = yc::model::TileMemKept();
  yc::model::TileMemTrimIdle();  // the window starts
  auto m = yc::model::NewTile<yc::model::MetaTile>();
  const void* hot = m.get();
  m.reset();                     // a small block freed just now, which the next small allocation wants
  const size_t both = yc::model::TileMemKept();
  CHECK(both > big);
  yc::model::TileMemTrimIdle();  // the window ends: the big block goes, the small one stays
  CHECK(yc::model::TileMemKept() == both - big);
  auto m2 = yc::model::NewTile<yc::model::MetaTile>();
  CHECK(static_cast<const void*>(m2.get()) == hot);
  m2.reset();
  yc::model::TileMemTrimIdle();
  yc::model::TileMemTrimIdle();
}
