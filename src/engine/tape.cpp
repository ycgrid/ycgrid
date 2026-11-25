// SPDX-License-Identifier: Apache-2.0
// tape.cpp - [YC] .yctape recordings (see tape.h)
#include "engine/tape.h"

#include "core/i18n.h"
#include "engine/bind.h"

#include <share.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

namespace yc::engine {

namespace {

#pragma pack(push, 1)
struct TapeFileHeader {
  char magic[8];
  uint32_t version;
  uint32_t header_size;
  int64_t start_unix_ms;
  int64_t reserved;
};
struct TapeRecord {
  int64_t t_ns;
  uint32_t len;
  uint32_t kind;
};
#pragma pack(pop)
static_assert(sizeof(TapeFileHeader) == kTapeHeader);
static_assert(sizeof(TapeRecord) == 16);

uint64_t Pad8(uint64_t n) { return (8 - (n & 7)) & 7; }

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool TapeWriter::Open(const std::wstring& path, int64_t start_unix_ms, std::string* error) {
  Close();
  // others may read a recording in progress (a copy, a replay elsewhere), nobody else writes it
  f_ = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
  if (!f_) {
    f_ = nullptr;
    if (error) *error = TR("The recording file couldn't be created");
    return false;
  }
  std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
  TapeFileHeader h{};
  std::memcpy(h.magic, kTapeMagic, sizeof(h.magic));
  h.version = kTapeVersion;
  h.header_size = kTapeHeader;
  h.start_unix_ms = start_unix_ms;
  std::fwrite(&h, sizeof(h), 1, f_);
  std::fflush(f_);  // a recording cut short by a crash still opens (its frames up to the last flush)
  last_flush_ns_ = SteadyNs();
  frames_ = 0;
  bytes_ = sizeof(h);
  return true;
}

bool TapeWriter::Write(int64_t t_ns, const uint8_t* data, size_t len, uint32_t kind) {
  if (!f_ || len > 0xFFFFFFFFu) return false;
  const TapeRecord r{t_ns, static_cast<uint32_t>(len), kind};
  static const uint8_t zeros[8] = {};
  const uint64_t pad = Pad8(len);
  if (std::fwrite(&r, sizeof(r), 1, f_) != 1 || std::fwrite(data, 1, len, f_) != len || std::fwrite(zeros, 1, pad, f_) != pad) return false;
  ++frames_;
  bytes_ += sizeof(r) + len + pad;
  const int64_t now = SteadyNs();
  if (now - last_flush_ns_ >= flush_every_ns) {
    std::fflush(f_);
    last_flush_ns_ = now;
  }
  return true;
}

std::vector<uint8_t> EncodeTopicBatch(const std::vector<std::pair<std::string, core::Value>>& items) {
  std::vector<uint8_t> out;
  auto put = [&](const void* p, size_t n) { out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
  const uint32_t n = static_cast<uint32_t>(items.size());
  put(&n, 4);
  for (const auto& [name, v] : items) {
    const uint16_t nl = static_cast<uint16_t>(std::min<size_t>(name.size(), 0xFFFF));
    put(&nl, 2);
    put(name.data(), nl);
    if (v.IsNumeric()) {
      const uint8_t t = 1;
      const double d = v.AsDouble();
      put(&t, 1);
      put(&d, 8);
    } else if (v.IsString()) {
      const uint8_t t = 2;
      const std::string_view s = v.str();
      const uint32_t sl = static_cast<uint32_t>(s.size());
      put(&t, 1);
      put(&sl, 4);
      put(s.data(), sl);
    } else if (v.IsBool()) {
      const uint8_t t = 3, b = v.boolean() ? 1 : 0;
      put(&t, 1);
      put(&b, 1);
    } else if (v.IsError()) {
      const uint8_t t = 4, e = static_cast<uint8_t>(v.error());
      put(&t, 1);
      put(&e, 1);
    } else {
      const uint8_t t = 0;
      put(&t, 1);
    }
  }
  return out;
}

bool DecodeTopicBatch(const uint8_t* data, size_t size, std::vector<std::pair<std::string, core::Value>>* out) {
  size_t i = 0;
  auto get = [&](void* p, size_t n) {
    if (i + n > size) return false;
    std::memcpy(p, data + i, n);
    i += n;
    return true;
  };
  uint32_t n = 0;
  if (!get(&n, 4) || n > size) return false;
  out->clear();
  out->reserve(n);
  for (uint32_t k = 0; k < n; ++k) {
    uint16_t nl = 0;
    if (!get(&nl, 2) || i + nl > size) return false;
    std::string name(reinterpret_cast<const char*>(data + i), nl);
    i += nl;
    uint8_t t = 0;
    if (!get(&t, 1)) return false;
    core::Value v;
    if (t == 1) {
      double d = 0;
      if (!get(&d, 8)) return false;
      v = core::Value::Number(d);
    } else if (t == 2) {
      uint32_t sl = 0;
      if (!get(&sl, 4) || i + sl > size) return false;
      v = core::Value::String(std::string_view(reinterpret_cast<const char*>(data + i), sl));
      i += sl;
    } else if (t == 3 || t == 4) {
      uint8_t b = 0;
      if (!get(&b, 1)) return false;
      v = t == 3 ? core::Value::Bool(b != 0) : core::Value::Error(static_cast<core::Err>(b));
    } else if (t != 0) {
      return false;
    }
    out->emplace_back(std::move(name), std::move(v));
  }
  return i == size;
}

void TapeWriter::Close() {
  if (f_) std::fclose(f_);
  f_ = nullptr;
}

bool TapeReader::Open(const std::wstring& path, std::string* error) {
  Close();
  auto fail = [&](const std::string& why) {
    Close();
    if (error) *error = why;
    return false;
  };
  f_ = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);  // a recording still being written opens too
  if (!f_) {
    f_ = nullptr;
    return fail(TR("The recording couldn't be opened"));
  }
  TapeFileHeader h{};
  if (std::fread(&h, sizeof(h), 1, f_) != 1 || std::memcmp(h.magic, kTapeMagic, sizeof(h.magic)) != 0 || h.header_size < kTapeHeader)
    return fail(TR("This isn't a YCGrid recording"));
  if (h.version > kTapeVersion) return fail(TR("This recording was made by a newer YCGrid"));
  idx_ = TapeIndex{};
  idx_.start_unix_ms = h.start_unix_ms;
  _fseeki64(f_, 0, SEEK_END);
  const uint64_t size = static_cast<uint64_t>(_ftelli64(f_));
  uint64_t pos = h.header_size;
  std::set<std::string> seen_topics;
  while (pos + sizeof(TapeRecord) <= size) {
    TapeRecord r{};
    _fseeki64(f_, static_cast<int64_t>(pos), SEEK_SET);
    if (std::fread(&r, sizeof(r), 1, f_) != 1) break;
    const uint64_t body = pos + sizeof(r);
    if (body + r.len > size) break;  // cut short
    if (r.kind == kTapeFrame && r.len >= sizeof(FrameHeader)) {
      FrameHeader fh{};
      if (std::fread(&fh, sizeof(fh), 1, f_) != 1) break;
      std::string name;
      if (fh.name_off <= r.len && fh.name_len <= r.len - fh.name_off && fh.name_len < 4096) {
        name.resize(fh.name_len);
        _fseeki64(f_, static_cast<int64_t>(body + fh.name_off), SEEK_SET);
        if (fh.name_len && std::fread(name.data(), 1, name.size(), f_) != name.size()) break;
      }
      size_t ti = 0;
      while (ti < idx_.tables.size() && idx_.tables[ti] != name) ++ti;
      if (ti == idx_.tables.size()) {
        if (ti >= 0xFFFF) break;
        idx_.tables.push_back(name);
      }
      idx_.table.push_back(static_cast<uint16_t>(ti));
      // times only go forward (a clock step while recording must not reorder the replay)
      idx_.t.push_back(idx_.t.empty() ? r.t_ns : std::max(r.t_ns, idx_.t.back()));
      idx_.off.push_back(body);
      idx_.len.push_back(r.len);
    } else if (r.kind == kTapePlot) {
      idx_.table.push_back(TapeIndex::kPlotRecord);
      idx_.t.push_back(idx_.t.empty() ? r.t_ns : std::max(r.t_ns, idx_.t.back()));
      idx_.off.push_back(body);
      idx_.len.push_back(r.len);
    } else if (r.kind == kTapeMark && r.len < 65536) {
      std::string text(r.len, '\0');
      if (r.len && std::fread(text.data(), 1, text.size(), f_) != text.size()) break;
      TapeIndex::Mark m;
      m.t = r.t_ns;
      const size_t tab = text.find('\t');
      m.kind = tab == std::string::npos ? std::string("note") : text.substr(0, tab);
      m.text = tab == std::string::npos ? text : text.substr(tab + 1);
      idx_.marks.push_back(std::move(m));
    } else if (r.kind == kTapeTopics) {
      std::vector<uint8_t> buf(r.len);
      if (r.len && std::fread(buf.data(), 1, buf.size(), f_) != buf.size()) break;
      std::vector<std::pair<std::string, core::Value>> items;
      if (DecodeTopicBatch(buf.data(), buf.size(), &items)) {
        for (const auto& it : items) seen_topics.insert(it.first);
        idx_.table.push_back(TapeIndex::kTopicRecord);
        idx_.t.push_back(idx_.t.empty() ? r.t_ns : std::max(r.t_ns, idx_.t.back()));
        idx_.off.push_back(body);
        idx_.len.push_back(r.len);
      }
    }
    pos = body + r.len + Pad8(r.len);
  }
  idx_.topics.assign(seen_topics.begin(), seen_topics.end());
  return true;
}

bool TapeReader::Frame(size_t i, std::vector<uint8_t>* out) {
  if (!f_ || i >= idx_.off.size()) return false;
  out->resize(idx_.len[i]);
  _fseeki64(f_, static_cast<int64_t>(idx_.off[i]), SEEK_SET);
  return std::fread(out->data(), 1, out->size(), f_) == out->size();
}

void TapeReader::Close() {
  if (f_) std::fclose(f_);
  f_ = nullptr;
}

}  // namespace yc::engine
