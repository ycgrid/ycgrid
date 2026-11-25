// SPDX-License-Identifier: Apache-2.0
// geo.cpp - decoding the embedded region shapes (scripts/build_map_data.py) and finding regions by name
#include "engine/geo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace yc::engine {

namespace {

const unsigned char kGeoData[] = {
#include "engine/geo_data.inc"
};

struct Index {
  std::vector<GeoRegion> regions;
  std::unordered_multimap<std::string, int> exact;  // GeoKey(name) -> region
  std::unordered_multimap<std::string, int> loose;  // without "city", "county", 市, 縣...
};

// names without a trailing administrative word, so "Taipei" finds "Taipei City" and 台北 finds 台北市
std::string Loosen(const std::string& key) {
  static const char* kSuffix[] = {" city", " county", " province", " prefecture", " state", " region", " district", " municipality",
                                  "\xE5\xB8\x82" /* 市 */, "\xE7\xB8\xA3" /* 縣 */, "\xE5\x8E\xBF" /* 县 */, "\xE7\x9C\x81" /* 省 */,
                                  "\xE5\xB7\x9E" /* 州 */, "\xE9\x83\xBD" /* 都 */, "\xE5\xBA\x9C" /* 府 */, "\xE7\x9C\x8C" /* 県 */};
  for (const char* s : kSuffix) {
    const size_t n = std::strlen(s);
    if (key.size() > n + 2 && key.compare(key.size() - n, n, s) == 0) return key.substr(0, key.size() - n);
  }
  return {};
}

const Index& Load() {
  static Index idx;
  static std::once_flag once;
  std::call_once(once, [] {
    const unsigned char* p = kGeoData;
    const unsigned char* end = kGeoData + sizeof(kGeoData);
    if (end - p < 10 || std::memcmp(p, "YCGEO1", 6) != 0) return;
    p += 6;
    auto u8 = [&]() -> uint8_t { return p < end ? *p++ : 0; };
    auto u16 = [&]() -> uint16_t {
      const uint16_t a = u8();
      return static_cast<uint16_t>(a | (u8() << 8));
    };
    auto i16 = [&]() { return static_cast<int16_t>(u16()); };
    auto str = [&]() {
      const uint8_t n = u8();
      std::string s(reinterpret_cast<const char*>(p), std::min<size_t>(n, static_cast<size_t>(end - p)));
      p += s.size();
      return s;
    };
    const uint32_t count = static_cast<uint32_t>(u16()) | (static_cast<uint32_t>(u16()) << 16);
    idx.regions.reserve(count);
    for (uint32_t r = 0; r < count && p < end; ++r) {
      GeoRegion g;
      g.level = u8();
      g.country = str();
      g.iso = str();
      const uint8_t nn = u8();
      for (uint8_t k = 0; k < nn; ++k) g.names.push_back(str());
      g.label_lon = i16() / 100.0f;
      g.label_lat = i16() / 100.0f;
      const uint16_t parts = u16();
      double best_area = -1;
      for (uint16_t q = 0; q < parts && p < end; ++q) {
        const uint16_t np = u16();
        std::vector<std::pair<float, float>> ring;
        ring.reserve(np);
        int x = i16(), y = i16();
        ring.emplace_back(x / 100.0f, y / 100.0f);
        for (uint16_t k = 1; k < np && p < end; ++k) {
          const int8_t dx = static_cast<int8_t>(u8());
          if (dx == -128) {
            x = i16();
            y = i16();
          } else {
            x += dx;
            y += static_cast<int8_t>(u8());
          }
          ring.emplace_back(x / 100.0f, y / 100.0f);
        }
        float a0 = 1e9f, a1 = -1e9f, b0 = 1e9f, b1 = -1e9f;
        double area = 0;
        for (size_t k = 0; k < ring.size(); ++k) {
          const auto& [lx, ly] = ring[k];
          a0 = std::min(a0, lx), a1 = std::max(a1, lx), b0 = std::min(b0, ly), b1 = std::max(b1, ly);
          const auto& [nx, ny] = ring[(k + 1) % ring.size()];
          area += static_cast<double>(lx) * ny - static_cast<double>(nx) * ly;
        }
        if (std::fabs(area) > best_area) {
          best_area = std::fabs(area);
          g.lon0 = a0, g.lon1 = a1, g.lat0 = b0, g.lat1 = b1;
        }
        g.parts.push_back(std::move(ring));
      }
      const int id = static_cast<int>(idx.regions.size());
      for (const auto& n : g.names) {
        const std::string key = GeoKey(n);
        idx.exact.emplace(key, id);
        if (std::string l = Loosen(key); !l.empty()) idx.loose.emplace(l, id);
      }
      idx.regions.push_back(std::move(g));
    }
  });
  return idx;
}

}  // namespace

const std::vector<GeoRegion>& GeoRegions() { return Load().regions; }

std::string GeoKey(std::string_view name) {
  std::string o;
  bool space = false;
  for (size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '.' || c == ',' || c == '-' || c == '\'') {
      space = !o.empty();
      continue;
    }
    if (space) {
      o += ' ';
      space = false;
    }
    // 臺 (E8 87 BA) -> 台 (E5 8F B0)
    if (c == 0xE8 && i + 2 < name.size() && static_cast<unsigned char>(name[i + 1]) == 0x87 && static_cast<unsigned char>(name[i + 2]) == 0xBA) {
      o += "\xE5\x8F\xB0";
      i += 2;
      continue;
    }
    o += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
  }
  return o;
}

int FindGeoRegion(std::string_view name, int level, std::string_view country, bool* ambiguous) {
  const Index& idx = Load();
  if (ambiguous) *ambiguous = false;
  const std::string key = GeoKey(name);
  if (key.empty()) return -1;
  auto pick = [&](const std::unordered_multimap<std::string, int>& m, const std::string& k) {
    std::vector<int> found;
    auto [a, b] = m.equal_range(k);
    for (auto it = a; it != b; ++it) {
      const GeoRegion& g = idx.regions[static_cast<size_t>(it->second)];
      if (level >= 0 && g.level != level) continue;
      if (!country.empty() && g.country != country && !(g.level == 0 && g.country == country)) continue;
      if (std::find(found.begin(), found.end(), it->second) == found.end()) found.push_back(it->second); // lesson-ok: find-push the regions of one name
    }
    return found;
  };
  std::vector<int> f = pick(idx.exact, key);
  if (f.empty()) f = pick(idx.loose, key);
  if (f.empty()) {
    if (const std::string l = Loosen(key); !l.empty()) {
      f = pick(idx.exact, l);
      if (f.empty()) f = pick(idx.loose, l);
    }
  }
  if (f.empty()) return -1;
  // a country before a subdivision of the same name (Georgia, the country, unless a level or country says otherwise)
  // ...and a region whose own (first) name is meant before one that only has it as an alternative name
  auto primary = [&](int r) {
    const std::string own = GeoKey(idx.regions[static_cast<size_t>(r)].names.empty() ? std::string() : idx.regions[static_cast<size_t>(r)].names[0]);
    return own == key || Loosen(own) == key ? 0 : 1;
  };
  std::stable_sort(f.begin(), f.end(), [&](int x, int y) {
    const auto& a = idx.regions[static_cast<size_t>(x)];
    const auto& b = idx.regions[static_cast<size_t>(y)];
    if (a.level != b.level) return a.level < b.level;
    return primary(x) < primary(y);
  });
  if (f.size() > 1 && ambiguous && idx.regions[static_cast<size_t>(f[0])].level == idx.regions[static_cast<size_t>(f[1])].level && primary(f[0]) == primary(f[1]))
    *ambiguous = true;
  return f[0];
}

}  // namespace yc::engine
