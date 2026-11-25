// SPDX-License-Identifier: Apache-2.0
// bind.cpp - decoding column frames (see bind.h)
#include "engine/bind.h"

#include <windows.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace yc::engine {

using core::Value;

namespace {

bool InRange(uint64_t off, uint64_t len, size_t size) { return off <= size && len <= size - off; }

bool Valid(const uint8_t* bitmap, uint32_t i) { return !bitmap || (bitmap[i >> 3] >> (i & 7)) & 1; }

}  // namespace

bool DecodeColumnFrame(const uint8_t* data, size_t size, ColumnFrame* out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (size < sizeof(FrameHeader)) return fail("The frame is shorter than its header");
  FrameHeader h;
  std::memcpy(&h, data, sizeof h);
  if (h.magic != kFrameMagic) return fail("Not a column frame (bad magic number)");
  if (h.version != kFrameVersion) return fail("Unsupported column frame version");
  if (h.total_len > size) return fail("The frame is longer than the data that was sent");
  const size_t total = static_cast<size_t>(h.total_len);
  if (h.ncols > 4096) return fail("Too many columns in one frame");
  const uint64_t desc_end = sizeof(FrameHeader) + uint64_t{h.ncols} * sizeof(ColumnDesc);
  if (desc_end > total) return fail("The column descriptors don't fit in the frame");
  if (h.op < 1 || h.op > 4) return fail("Unknown frame operation");
  if (!InRange(h.name_off, h.name_len, total)) return fail("The table name is outside the frame");
  out->op = static_cast<FrameOp>(h.op);
  out->table.assign(reinterpret_cast<const char*>(data + h.name_off), h.name_len);
  out->key_col = h.key_col == 0xFFFFFFFFu ? -1 : static_cast<int>(h.key_col);
  if (out->key_col >= static_cast<int>(h.ncols)) return fail("The key column index is out of range");
  out->nrows = h.nrows;
  out->sent_ns = h.sent_ns;
  out->names.assign(h.ncols, {});
  out->cols.assign(h.ncols, {});
  const uint32_t n = h.nrows;
  for (uint32_t c = 0; c < h.ncols; ++c) {
    ColumnDesc d;
    std::memcpy(&d, data + sizeof(FrameHeader) + c * sizeof(ColumnDesc), sizeof d);
    if (!InRange(d.name_off, d.name_len, total)) return fail("A column name is outside the frame");
    out->names[c].assign(reinterpret_cast<const char*>(data + d.name_off), d.name_len);
    const uint8_t* valid = nullptr;
    if (d.validity_off) {
      if (!InRange(d.validity_off, (uint64_t{n} + 7) / 8, total)) return fail("A validity bitmap is outside the frame");
      valid = data + d.validity_off;
    }
    auto& col = out->cols[c];
    col.resize(n);
    switch (static_cast<ColType>(d.type)) {
      case ColType::Float64: {
        if (d.values_len < uint64_t{n} * 8 || !InRange(d.values_off, uint64_t{n} * 8, total)) return fail("A number column is shorter than the rows");
        const uint8_t* p = data + d.values_off;
        for (uint32_t i = 0; i < n; ++i) {
          if (!Valid(valid, i)) continue;
          double x;
          std::memcpy(&x, p + uint64_t{i} * 8, 8);
          if (!std::isnan(x)) col[i] = Value::Number(x);  // NaN = no value (pandas' missing number)
        }
        break;
      }
      case ColType::Int64: {
        if (d.values_len < uint64_t{n} * 8 || !InRange(d.values_off, uint64_t{n} * 8, total)) return fail("An integer column is shorter than the rows");
        const uint8_t* p = data + d.values_off;
        for (uint32_t i = 0; i < n; ++i) {
          if (!Valid(valid, i)) continue;
          int64_t x;
          std::memcpy(&x, p + uint64_t{i} * 8, 8);
          col[i] = Value::Number(static_cast<double>(x));
        }
        break;
      }
      case ColType::Bool: {
        if (!InRange(d.values_off, (uint64_t{n} + 7) / 8, total)) return fail("A boolean column is shorter than the rows");
        const uint8_t* p = data + d.values_off;
        for (uint32_t i = 0; i < n; ++i) {
          if (Valid(valid, i)) col[i] = Value::Bool((p[i >> 3] >> (i & 7)) & 1);
        }
        break;
      }
      case ColType::Utf8: {
        if (!InRange(d.offsets_off, (uint64_t{n} + 1) * 4, total)) return fail("A text column's offsets are outside the frame");
        const uint8_t* offs = data + d.offsets_off;
        auto at = [&](uint32_t i) {
          int32_t o;
          std::memcpy(&o, offs + uint64_t{i} * 4, 4);
          return o;
        };
        if (!InRange(d.values_off, d.values_len, total)) return fail("A text column's bytes are outside the frame");
        const char* bytes = reinterpret_cast<const char*>(data + d.values_off);
        for (uint32_t i = 0; i < n; ++i) {
          const int32_t a = at(i), b = at(i + 1);
          if (a < 0 || b < a || static_cast<uint64_t>(b) > d.values_len) return fail("A text column's offsets are out of order");
          if (Valid(valid, i)) col[i] = Value::String(std::string_view(bytes + a, static_cast<size_t>(b - a)));
        }
        break;
      }
      case ColType::Null: break;
      default: return fail("Unknown column type");
    }
  }
  return true;
}

bool DecodeRawFrame(const uint8_t* data, size_t size, RawFrame* out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (size < sizeof(FrameHeader)) return fail("The frame is shorter than its header");
  FrameHeader h;
  std::memcpy(&h, data, sizeof h);
  if (h.magic != kFrameMagic || h.version != kFrameVersion) return fail("Not a column frame");
  if (h.total_len > size) return fail("The frame is longer than the data that was sent");
  const size_t total = static_cast<size_t>(h.total_len);
  if (h.ncols > 4096 || sizeof(FrameHeader) + uint64_t{h.ncols} * sizeof(ColumnDesc) > total) return fail("Bad column count");
  if (!InRange(h.name_off, h.name_len, total)) return fail("The name is outside the frame");
  out->table.assign(reinterpret_cast<const char*>(data + h.name_off), h.name_len);
  out->op = static_cast<FrameOp>(h.op);
  out->nrows = h.nrows;
  out->sent_ns = h.sent_ns;
  const uint32_t n = h.nrows;
  out->names.assign(h.ncols, {});
  out->num.assign(h.ncols, {});
  out->text.assign(h.ncols, {});
  for (uint32_t c = 0; c < h.ncols; ++c) {
    ColumnDesc d;
    std::memcpy(&d, data + sizeof(FrameHeader) + c * sizeof(ColumnDesc), sizeof d);
    if (!InRange(d.name_off, d.name_len, total)) return fail("A column name is outside the frame");
    out->names[c].assign(reinterpret_cast<const char*>(data + d.name_off), d.name_len);
    const uint8_t* valid = nullptr;
    if (d.validity_off) {
      if (!InRange(d.validity_off, (uint64_t{n} + 7) / 8, total)) return fail("A validity bitmap is outside the frame");
      valid = data + d.validity_off;
    }
    const auto type = static_cast<ColType>(d.type);
    if (type == ColType::Utf8) {
      if (!InRange(d.offsets_off, (uint64_t{n} + 1) * 4, total) || !InRange(d.values_off, d.values_len, total)) return fail("A text column is outside the frame");
      auto& col = out->text[c];
      col.resize(n);
      const char* bytes = reinterpret_cast<const char*>(data + d.values_off);
      for (uint32_t i = 0; i < n; ++i) {
        int32_t a, b;
        std::memcpy(&a, data + d.offsets_off + uint64_t{i} * 4, 4);
        std::memcpy(&b, data + d.offsets_off + uint64_t{i + 1} * 4, 4);
        if (a < 0 || b < a || static_cast<uint64_t>(b) > d.values_len) return fail("A text column's offsets are out of order");
        if (Valid(valid, i)) col[i].assign(bytes + a, static_cast<size_t>(b - a));
      }
      continue;
    }
    auto& col = out->num[c];
    col.assign(n, std::numeric_limits<double>::quiet_NaN());
    if (type == ColType::Float64 || type == ColType::Int64) {
      if (!InRange(d.values_off, uint64_t{n} * 8, total)) return fail("A number column is shorter than the rows");
      if (type == ColType::Float64) {
        std::memcpy(col.data(), data + d.values_off, size_t{n} * 8);
      } else {
        for (uint32_t i = 0; i < n; ++i) {
          int64_t x;
          std::memcpy(&x, data + d.values_off + uint64_t{i} * 8, 8);
          col[i] = static_cast<double>(x);
        }
      }
      if (valid) {
        for (uint32_t i = 0; i < n; ++i) {
          if (!Valid(valid, i)) col[i] = std::numeric_limits<double>::quiet_NaN();
        }
      }
    } else if (type == ColType::Bool) {
      if (!InRange(d.values_off, (uint64_t{n} + 7) / 8, total)) return fail("A boolean column is shorter than the rows");
      for (uint32_t i = 0; i < n; ++i) {
        if (Valid(valid, i)) col[i] = ((data[d.values_off + (i >> 3)] >> (i & 7)) & 1) ? 1.0 : 0.0;
      }
    }
  }
  return true;
}

std::vector<uint8_t> EncodeColumnFrame(const ColumnFrame& f) {
  const uint32_t nc = static_cast<uint32_t>(f.cols.size()), n = f.nrows;
  std::vector<uint8_t> out(sizeof(FrameHeader) + nc * sizeof(ColumnDesc), 0);
  auto align = [&] {
    while (out.size() % 8) out.push_back(0);
  };
  auto put = [&](const void* p, size_t len) {
    align();
    const uint64_t off = out.size();
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + len);
    return off;
  };
  FrameHeader h{};
  h.magic = kFrameMagic;
  h.version = kFrameVersion;
  h.op = static_cast<uint16_t>(f.op);
  h.ncols = nc;
  h.nrows = n;
  h.key_col = f.key_col < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(f.key_col);
  h.sent_ns = f.sent_ns;
  h.name_off = static_cast<uint32_t>(put(f.table.data(), f.table.size()));
  h.name_len = static_cast<uint32_t>(f.table.size());
  std::vector<ColumnDesc> descs(nc);
  for (uint32_t c = 0; c < nc; ++c) {
    ColumnDesc d{};
    const std::string& name = c < f.names.size() ? f.names[c] : std::string();
    d.name_off = static_cast<uint32_t>(put(name.data(), name.size()));
    d.name_len = static_cast<uint32_t>(name.size());
    const auto& col = f.cols[c];
    bool any_text = false, all_bool = true, any = false;
    for (const Value& v : col) {
      if (v.IsEmpty()) continue;
      any = true;
      any_text = any_text || v.IsString() || v.IsError();
      all_bool = all_bool && v.IsBool();
    }
    std::vector<uint8_t> valid((n + 7) / 8, 0);
    bool all_valid = true;
    for (uint32_t i = 0; i < n; ++i) {
      if (i < col.size() && !col[i].IsEmpty()) valid[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      else all_valid = false;
    }
    if (!any) {
      d.type = static_cast<uint8_t>(ColType::Null);
    } else if (any_text) {
      d.type = static_cast<uint8_t>(ColType::Utf8);
      std::string bytes;
      std::vector<int32_t> offs(n + 1, 0);
      for (uint32_t i = 0; i < n; ++i) {
        if (i < col.size() && !col[i].IsEmpty()) bytes += col[i].IsString() ? std::string(col[i].str()) : BindKeyText(col[i]);
        offs[i + 1] = static_cast<int32_t>(bytes.size());
      }
      d.offsets_off = put(offs.data(), offs.size() * 4);
      d.values_off = put(bytes.data(), bytes.size());
      d.values_len = bytes.size();
    } else if (all_bool) {
      d.type = static_cast<uint8_t>(ColType::Bool);
      std::vector<uint8_t> bits((n + 7) / 8, 0);
      for (uint32_t i = 0; i < n && i < col.size(); ++i) {
        if (col[i].IsBool() && col[i].boolean()) bits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      }
      d.values_off = put(bits.data(), bits.size());
      d.values_len = bits.size();
    } else {
      d.type = static_cast<uint8_t>(ColType::Float64);
      std::vector<double> xs(n, 0.0);
      for (uint32_t i = 0; i < n && i < col.size(); ++i) {
        if (col[i].IsNumeric()) xs[i] = col[i].AsDouble();
        else if (col[i].IsBool()) xs[i] = col[i].boolean() ? 1 : 0;
      }
      d.values_off = put(xs.data(), xs.size() * 8);
      d.values_len = xs.size() * 8;
    }
    if (!all_valid && any) d.validity_off = put(valid.data(), valid.size());
    descs[c] = d;
  }
  align();
  h.total_len = out.size();
  std::memcpy(out.data(), &h, sizeof h);
  for (uint32_t c = 0; c < nc; ++c) std::memcpy(out.data() + sizeof(FrameHeader) + c * sizeof(ColumnDesc), &descs[c], sizeof(ColumnDesc));
  return out;
}

std::string BindKeyText(const Value& v) {
  if (v.IsString()) return std::string(v.str());
  if (v.IsNumeric()) {
    const double d = v.AsDouble();
    if (d == std::floor(d) && std::fabs(d) < 9.0e15) return std::to_string(static_cast<int64_t>(d));
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, d);
    return std::string(b, r.ptr);
  }
  if (v.IsBool()) return v.boolean() ? "TRUE" : "FALSE";
  return {};
}

int64_t PerfCounterNs() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c;
  ::QueryPerformanceCounter(&c);
  // like CPython: ticks * 1e9 / frequency without overflowing
  const int64_t q = c.QuadPart / freq, r = c.QuadPart % freq;
  return q * 1000000000LL + r * 1000000000LL / freq;
}

}  // namespace yc::engine
