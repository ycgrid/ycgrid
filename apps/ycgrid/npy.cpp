// SPDX-License-Identifier: Apache-2.0
// npy.cpp - see npy.h. Format: "\x93NUMPY", major, minor, header length (u16 for 1.x, u32 for 2.x / 3.x), a Python
// dict literal {'descr': '<f8', 'fortran_order': False, 'shape': (n,)}, then the data.
#include "npy.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace yc::app {

namespace {

std::string Field(const std::string& h, const char* key) {
  const size_t k = h.find(std::string("'") + key + "'");
  if (k == std::string::npos) return {};
  size_t v = h.find(':', k);
  if (v == std::string::npos) return {};
  ++v;
  while (v < h.size() && h[v] == ' ') ++v;
  if (v < h.size() && h[v] == '\'') {
    const size_t e = h.find('\'', v + 1);
    return e == std::string::npos ? std::string() : h.substr(v + 1, e - v - 1);
  }
  if (v < h.size() && h[v] == '(') {
    const size_t e = h.find(')', v);
    return e == std::string::npos ? std::string() : h.substr(v + 1, e - v - 1);
  }
  size_t e = v;
  while (e < h.size() && h[e] != ',' && h[e] != '}') ++e;
  return h.substr(v, e - v);
}

void Utf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) out->push_back(static_cast<char>(cp));
  else if (cp < 0x800) out->push_back(static_cast<char>(0xC0 | (cp >> 6))), out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// a dtype's item count ("8", "10"): digits only, false otherwise (numpy's object dtype "|O" has none)
bool Count(const std::string& s, uint64_t* out) {
  if (s.empty() || s.size() > 12) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *out = v;
  return true;
}

// a * b, false when it doesn't fit in 64 bits
bool Mul(uint64_t a, uint64_t b, uint64_t* out) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a) return false;
  *out = a * b;
  return true;
}

double Half(uint16_t h) {  // IEEE 754 half precision (numpy float16)
  const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
  const double sign = (h & 0x8000) ? -1.0 : 1.0;
  if (e == 0) return sign * std::ldexp(m, -24);
  if (e == 31) return m ? std::numeric_limits<double>::quiet_NaN() : sign * std::numeric_limits<double>::infinity();
  return sign * std::ldexp(m + 1024, e - 25);
}

std::mutex g_pre_mu;
std::map<std::pair<std::wstring, int>, std::shared_ptr<NpyColumn>> g_pre;

bool ReadNpyFile(const std::wstring& path, NpyColumn* out, std::string* error, int column);

}  // namespace

bool ReadNpy(const std::wstring& path, NpyColumn* out, std::string* error, int column) {
  std::shared_ptr<NpyColumn> pre;
  {
    std::lock_guard<std::mutex> lk(g_pre_mu);
    auto it = g_pre.find({path, column});
    if (it != g_pre.end()) {
      pre = std::move(it->second);
      g_pre.erase(it);
    }
  }
  if (pre) {
    *out = std::move(*pre);
    return true;
  }
  return ReadNpyFile(path, out, error, column);
}

void NpyPreload::Add(const std::wstring& path, int column) {
  auto col = std::make_shared<NpyColumn>();
  std::string why;
  if (!ReadNpyFile(path, col.get(), &why, column)) return;  // read again (and the reason given) where it is used
  std::lock_guard<std::mutex> lk(g_pre_mu);
  g_pre[{path, column}] = std::move(col);
  keys_.emplace_back(path, column);
}

NpyPreload::~NpyPreload() {
  std::lock_guard<std::mutex> lk(g_pre_mu);
  for (const auto& k : keys_) g_pre.erase(k);
}

namespace {

bool ReadNpyFile(const std::wstring& path, NpyColumn* out, std::string* error, int column) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return fail("can't open the file");
  char magic[8];
  if (!f.read(magic, 8) || std::memcmp(magic, "\x93NUMPY", 6) != 0) return fail("not a .npy file");
  uint32_t hlen = 0;
  if (magic[6] == 1) {
    uint16_t h16 = 0;
    f.read(reinterpret_cast<char*>(&h16), 2);
    hlen = h16;
  } else {
    f.read(reinterpret_cast<char*>(&hlen), 4);
  }
  if (!f || hlen > (1u << 20)) return fail("bad .npy header");
  std::string h(hlen, '\0');
  f.read(h.data(), hlen);
  const std::string descr = Field(h, "descr"), shape = Field(h, "shape"), fortran = Field(h, "fortran_order");
  if (fortran.find("True") != std::string::npos) return fail("Fortran-order arrays aren't supported (use np.ascontiguousarray)");
  // shape: (n,) or (n, m)
  std::vector<uint64_t> dims;
  for (size_t p = 0; p < shape.size();) {
    while (p < shape.size() && (shape[p] < '0' || shape[p] > '9')) ++p;
    if (p >= shape.size()) break;
    uint64_t v = 0;
    while (p < shape.size() && shape[p] >= '0' && shape[p] <= '9') v = v * 10 + static_cast<uint64_t>(shape[p++] - '0');
    dims.push_back(v);
  }
  if (dims.empty() || dims.size() > 2) return fail("only 1-D and 2-D arrays are supported");
  const uint64_t n = dims[0], width = dims.size() == 2 ? dims[1] : 1;
  const int col = dims.size() == 2 ? std::max(0, column) : 0;
  if (static_cast<uint64_t>(col) >= width) return fail("no such column");
  if (descr.size() < 3) return fail("unknown dtype " + descr);
  const char order = descr[0], kind = descr[1];
  if (order == '>') return fail("big-endian arrays aren't supported");
  const std::string unit = descr.substr(2);
  out->text = out->date = false;
  out->num.clear();
  out->str.clear();
  // the data must be in the file: what the header says is never allocated before that is checked
  const std::streamoff data_at = f.tellg();
  f.seekg(0, std::ios::end);
  const std::streamoff file_end = f.tellg();
  f.seekg(data_at);
  if (data_at < 0 || file_end < data_at || !f) return fail("bad .npy header");
  const uint64_t avail = static_cast<uint64_t>(file_end - data_at);
  uint64_t item = 0;  // bytes per element
  const std::string count = kind == 'M' ? unit.substr(0, unit.find('[')) : unit;
  if (!Count(count, &item) || item == 0) return fail("unsupported dtype " + descr);
  const bool text = kind == 'U' || kind == 'S';
  if (kind == 'U' && !Mul(item, 4, &item)) return fail("unsupported dtype " + descr);
  const bool known = text || (kind == 'f' && (item == 2 || item == 4 || item == 8)) ||
                     ((kind == 'i' || kind == 'u') && (item == 1 || item == 2 || item == 4 || item == 8)) ||
                     (kind == 'b' && item == 1) || (kind == 'M' && item == 8);
  if (!known) return fail("unsupported dtype " + descr);
  uint64_t row_bytes = 0, need = 0;
  if (!Mul(item, width, &row_bytes) || !Mul(row_bytes, n, &need) || need > avail) return fail("the file is cut short");
  if (text) {  // fixed-width text: UTF-32 ('U') or bytes ('S', read as UTF-8)
    const uint64_t chars = kind == 'U' ? item / 4 : item;
    std::vector<char> row(static_cast<size_t>(row_bytes));
    out->text = true;
    out->str.reserve(static_cast<size_t>(n));
    for (uint64_t r = 0; r < n; ++r) {
      if (!f.read(row.data(), static_cast<std::streamsize>(row.size()))) return fail("the file is cut short");
      const char* at = row.data() + static_cast<size_t>(static_cast<uint64_t>(col) * item);
      std::string s;
      for (uint64_t k = 0; k < chars; ++k) {
        if (kind == 'S') {
          if (!at[k]) break;
          s.push_back(at[k]);
          continue;
        }
        uint32_t cp;
        std::memcpy(&cp, at + k * 4, 4);
        if (!cp) break;
        Utf8(&s, cp);
      }
      out->str.push_back(std::move(s));
    }
    return true;
  }
  const size_t size = static_cast<size_t>(item);
  out->date = kind == 'M';  // datetime64[unit]: to Unix seconds
  double date_scale = 1.0;
  if (out->date) {
    const size_t lb = unit.find('['), rb = unit.find(']');
    const std::string u = lb == std::string::npos ? "s" : unit.substr(lb + 1, rb - lb - 1);
    date_scale = u == "ns" ? 1e-9 : u == "us" ? 1e-6 : u == "ms" ? 1e-3 : u == "s" ? 1.0 : u == "m" ? 60.0 : u == "h" ? 3600.0 : u == "D" ? 86400.0 : 1.0;
  }
  std::vector<uint8_t> raw(static_cast<size_t>(need));
  if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) return fail("the file is cut short");
  out->num.resize(static_cast<size_t>(n));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (uint64_t r = 0; r < n; ++r) {
    const uint8_t* p = raw.data() + static_cast<size_t>(r * width + static_cast<uint64_t>(col)) * size;
    double v = nan;
    if (kind == 'f' && size == 8) std::memcpy(&v, p, 8);
    else if (kind == 'f' && size == 4) {
      float x;
      std::memcpy(&x, p, 4);
      v = x;
    } else if ((kind == 'i' || kind == 'M') && size == 8) {
      int64_t x;
      std::memcpy(&x, p, 8);
      v = kind == 'M' ? (x == std::numeric_limits<int64_t>::min() ? nan : static_cast<double>(x) * date_scale) : static_cast<double>(x);
    } else if (kind == 'i' && size == 4) {
      int32_t x;
      std::memcpy(&x, p, 4);
      v = x;
    } else if (kind == 'i' && size == 2) {
      int16_t x;
      std::memcpy(&x, p, 2);
      v = x;
    } else if (kind == 'i' && size == 1) {
      v = static_cast<int8_t>(*p);
    } else if (kind == 'u' && size == 2) {
      uint16_t x;
      std::memcpy(&x, p, 2);
      v = x;
    } else if (kind == 'u' && size == 1) {
      v = *p;
    } else if (kind == 'f' && size == 2) {
      uint16_t x;
      std::memcpy(&x, p, 2);
      v = Half(x);
    } else if (kind == 'u' && size == 8) {
      uint64_t x;
      std::memcpy(&x, p, 8);
      v = static_cast<double>(x);
    } else if (kind == 'u' && size == 4) {
      uint32_t x;
      std::memcpy(&x, p, 4);
      v = x;
    } else if (kind == 'b') {
      v = *p ? 1.0 : 0.0;
    } else {
      return fail("unsupported dtype " + descr);
    }
    out->num[static_cast<size_t>(r)] = v;
  }
  return true;
}

}  // namespace

}  // namespace yc::app
