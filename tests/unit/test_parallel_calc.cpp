// SPDX-License-Identifier: Apache-2.0
// Parallel recalculation (PLAN §7.3, §7.7): the waves must give exactly the same values as the single-threaded pass.
#include <catch2/catch_test_macros.hpp>

#include "core/parallel.h"
#include "engine/document.h"

#include <atomic>
#include <string>
#include <vector>

using yc::engine::Document;

namespace {

constexpr uint32_t kRows = 3000;

// a workbook mixing what real sheets contain: many independent formulas, a long chain, range sums over formulas,
// lookups, text, errors and a circular reference
void Build(Document& d) {
  const uint32_t sh = d.wb().sheets[0]->id;
  for (uint32_t r = 0; r < kRows; ++r) {
    const std::string row = std::to_string(r + 1);
    d.SetInput(sh, r, 0, std::to_string((r * 37) % 101 + 0.25));        // A: data
    d.SetInput(sh, r, 1, std::to_string((r * 13) % 7));                  // B: data
    d.SetInput(sh, r, 2, "=A" + row + "*2+B" + row);                     // C: independent
    d.SetInput(sh, r, 3, "=IF(C" + row + ">100,\"hi\",C" + row + "/3)");  // D: depends on C
    d.SetInput(sh, r, 4, r == 0 ? "=C1" : "=E" + std::to_string(r) + "+C" + row);  // E: running total (a chain)
    d.SetInput(sh, r, 5, "=SUM(C1:C" + row + ")");                       // F: growing range over formulas
    d.SetInput(sh, r, 6, "=B" + row + "/(B" + row + "-3)");                // G: #DIV/0! for some rows
  }
  d.SetInput(sh, 0, 8, "=SUM(C:C)");                        // I1: whole column
  d.SetInput(sh, 1, 8, "=VLOOKUP(17.25,A1:C3000,3,FALSE)");  // I2: lookup
  d.SetInput(sh, 2, 8, "=I4+1");                            // I3, I4: circular
  d.SetInput(sh, 3, 8, "=I3+1");
  d.SetInput(sh, 4, 8, "=E3000-F3000");                     // I5: end of the chain vs the sums
}

std::vector<yc::core::Value> Values(const Document& d) {
  const uint32_t sh = d.wb().sheets[0]->id;
  std::vector<yc::core::Value> v;
  for (uint32_t r = 0; r < kRows; ++r)
    for (uint32_t c = 0; c < 9; ++c) v.push_back(d.GetValue(sh, r, c));
  return v;
}

}  // namespace

TEST_CASE("ParallelFor covers every index exactly once") {
  std::vector<std::atomic<int>> hits(10007);
  yc::core::ParallelFor(hits.size(), 13, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) hits[i].fetch_add(1);
  });
  for (auto& h : hits) REQUIRE(h.load() == 1);
}

TEST_CASE("Parallel recalculation gives the same values as the serial pass") {
  Document serial, parallel;
  serial.calc_threads = 1;
  Build(serial);
  Build(parallel);
  serial.RecalcAll();
  parallel.RecalcAll();
  const auto a = Values(serial), b = Values(parallel);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    INFO("cell " << i / 9 << "," << i % 9);
    REQUIRE(a[i].SameAs(b[i]));
  }
  CHECK(serial.circular_cells.size() == parallel.circular_cells.size());
  if (yc::core::WorkerCount() > 1) {
    CHECK(parallel.last_stats().waves > 0);
    CHECK(parallel.last_stats().parallel > 0);
  }

  // an edit recalculates only the dependents, again identical
  const uint32_t s1 = serial.wb().sheets[0]->id, s2 = parallel.wb().sheets[0]->id;
  for (uint32_t r = 0; r < kRows; r += 3) {
    serial.SetInput(s1, r, 0, std::to_string(r % 17));
    parallel.SetInput(s2, r, 0, std::to_string(r % 17));
  }
  serial.Recalc();
  parallel.Recalc();
  const auto c = Values(serial), d = Values(parallel);
  for (size_t i = 0; i < c.size(); ++i) {
    INFO("after edit, cell " << i / 9 << "," << i % 9);
    REQUIRE(c[i].SameAs(d[i]));
  }
}

// timing only (hidden: yc_unit_tests "[bench]"): 200,000 formulas over market-data-like inputs
#include <chrono>
#include <cstdio>

TEST_CASE("Parallel recalculation speed", "[.][bench]") {
  auto run = [](int threads) {
    Document d;
    d.calc_threads = threads;
    const uint32_t sh = d.wb().sheets[0]->id;
    constexpr uint32_t rows = 40000;
    for (uint32_t r = 0; r < rows; ++r) {
      const std::string row = std::to_string(r + 1);
      d.SetInput(sh, r, 0, std::to_string(100 + r % 50));
      d.SetInput(sh, r, 1, std::to_string(101 + r % 50));
      d.SetInput(sh, r, 2, "=(A" + row + "+B" + row + ")/2");
      d.SetInput(sh, r, 3, "=B" + row + "-A" + row);
      d.SetInput(sh, r, 4, "=IF(D" + row + ">0,D" + row + "/C" + row + ",0)");
      d.SetInput(sh, r, 5, "=ROUND(C" + row + "*1.0123,2)&\" TWD\"");
      d.SetInput(sh, r, 6, "=MAX(A" + row + ":E" + row + ")+SQRT(ABS(D" + row + "))");
    }
    d.RecalcAll();
    const auto t0 = std::chrono::steady_clock::now();
    d.RecalcAll();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto& st = d.last_stats();
    std::printf("threads=%s: %u formulas in %.1f ms (%u in %u waves; waves: evaluate %.1f ms, write back %.1f ms)\n", threads == 1 ? "1" : "auto",
                st.evaluated, ms, st.parallel, st.waves, st.wave_eval_ms, st.wave_commit_ms);
    return ms;
  };
  const double serial = run(1), parallel = run(0);
  std::printf("speed-up x%.2f with %zu threads\n", serial / parallel, yc::core::WorkerCount());
}
