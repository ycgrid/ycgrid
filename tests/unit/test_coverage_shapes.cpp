// SPDX-License-Identifier: Apache-2.0
// Coverage of shape geometry (engine/shape_geometry.cpp): every autoshape's handles, for every shape that has them.
// Grabbing a handle and dropping it where it is changes nothing (a handle that jumps when it is clicked moves the
// shape under the user's mouse); a handle dragged anywhere keeps the adjustments finite and the shape drawable.
#include <catch2/catch_test_macros.hpp>

#include "engine/shape_geometry.h"

#include <cmath>

namespace eng = yc::engine;

TEST_CASE("Shapes coverage: every handle dropped where it is changes nothing; dragged anywhere stays drawable") {
  int with_handles = 0;
  for (int type = 1; type <= 200; ++type) {
    const auto defs = eng::AutoShapeAdjustDefaults(type);
    if (defs.empty()) continue;
    for (const auto& [w, h] : {std::pair<double, double>{120, 80}, {60, 140}}) {
      const auto hs = eng::AutoShapeHandles(type, w, h, defs);
      if (hs.empty()) continue;
      ++with_handles;
      for (size_t k = 0; k < hs.size(); ++k) {
        INFO("type " << type << " handle " << k << " size " << w << "x" << h);
        std::vector<double> adj = defs;
        REQUIRE(eng::AutoShapeDragHandle(type, k, w, h, hs[k], &adj));
        const auto again = eng::AutoShapeHandles(type, w, h, adj);
        REQUIRE(again.size() == hs.size());
        // angles are kept in whole degrees: a handle on a curve may settle up to half a degree away
        CHECK(std::abs(again[k].x - hs[k].x) < 1.0);
        CHECK(std::abs(again[k].y - hs[k].y) < 1.0);
        for (const eng::ShapePt p : {eng::ShapePt{0, 0}, eng::ShapePt{w, h}, eng::ShapePt{w / 2, h / 2}, eng::ShapePt{-w, 2 * h}}) {
          std::vector<double> moved = defs;
          eng::AutoShapeDragHandle(type, k, w, h, p, &moved);
          for (double a : moved) CHECK(std::isfinite(a));
          eng::ShapeGeom g;
          CHECK(eng::AutoShapeGeometry(type, w, h, &g, &moved));
        }
      }
    }
  }
  CHECK(with_handles > 60);
  std::vector<double> none;
  CHECK_FALSE(eng::AutoShapeDragHandle(1, 99, 100, 100, {1, 1}, &none));
}
