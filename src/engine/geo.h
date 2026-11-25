// SPDX-License-Identifier: Apache-2.0
// geo.h - the regions map charts draw (countries and their first-level subdivisions; Natural Earth, public domain)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yc::engine {

struct GeoRegion {
  uint8_t level = 0;            // 0 country, 1 state / province / county
  std::string country;          // ISO A3 of the country (its own code for a country)
  std::string iso;              // ISO 3166 code (country: alpha-2, subdivision: "TW-TPE")
  std::vector<std::string> names;
  float label_lon = 0, label_lat = 0;
  std::vector<std::vector<std::pair<float, float>>> parts;  // outer rings, (lon, lat) in degrees
  float lon0 = 0, lat0 = 0, lon1 = 0, lat1 = 0;              // bounds of the largest part
};

const std::vector<GeoRegion>& GeoRegions();

// how sheet text is compared with region names: case-insensitive, extra spaces dropped, 臺 written as 台
std::string GeoKey(std::string_view name);

// the region a name means: level -1 = any level; country = ISO A3 to look within ("" = any). -1 when none
// (several regions with that name and no country to choose: the first country's, or -1 when ambiguous = true)
int FindGeoRegion(std::string_view name, int level, std::string_view country, bool* ambiguous = nullptr);

}  // namespace yc::engine
