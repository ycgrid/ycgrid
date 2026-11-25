// SPDX-License-Identifier: Apache-2.0
// bind.h - [YC] column frames: how Python sends table data through shared memory (ADR 0007)
//
// A frame is one batch of rows for a bound table, laid out by column like Apache Arrow: per column a values buffer,
// for text an int32 offsets buffer (n + 1 entries) and UTF-8 bytes, and an optional validity bitmap (bit i set =
// row i has a value; least significant bit first). Booleans are bit-packed as in Arrow. Everything is little endian
// and every buffer is 8-byte aligned. The frame lives in a shared-memory ring; the control pipe only says where it is.
//
//   FrameHeader (64 bytes) | ColumnDesc x ncols (48 bytes each) | table name, column names | buffers
#pragma once

#include "core/value.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yc::engine {

inline constexpr uint32_t kFrameMagic = 0x46434359;  // "YCCF"
inline constexpr uint16_t kFrameVersion = 1;

enum class FrameOp : uint16_t {
  Upsert = 1,   // rows by key: changed columns of existing keys, new keys append rows
  Replace = 2,  // the table's rows become exactly these (in this order)
  Delete = 3,   // remove the rows of these keys (only the key column is sent)
  Append = 4,   // add rows at the end (no key)
};

enum class ColType : uint8_t { Float64 = 1, Int64 = 2, Utf8 = 3, Bool = 4, Null = 5 };

#pragma pack(push, 1)
struct FrameHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t op;
  uint32_t ncols;
  uint32_t nrows;
  uint32_t key_col;   // index of the key column in this frame; 0xFFFFFFFF = none
  uint32_t name_off;  // table name (UTF-8) offset and length inside the frame
  uint32_t name_len;
  uint32_t reserved0;
  int64_t sent_ns;    // the sender's QueryPerformanceCounter time in ns (Python's time.perf_counter_ns()); 0 = unknown
  uint64_t total_len;
  uint8_t reserved[16];
};
struct ColumnDesc {
  uint32_t name_off, name_len;
  uint8_t type;  // ColType
  uint8_t pad[7];
  uint64_t values_off, values_len;
  uint64_t offsets_off;   // Utf8: int32 offsets
  uint64_t validity_off;  // 0 = every row has a value
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 64);
static_assert(sizeof(ColumnDesc) == 48);

// a decoded frame: values by column (copied out of the shared memory)
struct ColumnFrame {
  FrameOp op = FrameOp::Upsert;
  std::string table;
  int key_col = -1;
  uint32_t nrows = 0;
  int64_t sent_ns = 0;
  std::vector<std::string> names;
  std::vector<std::vector<core::Value>> cols;  // [column][row]; an empty Value = no value
};

// decodes a frame; false with the reason when it is malformed (out-of-range offsets, unknown types...)
bool DecodeColumnFrame(const uint8_t* data, size_t size, ColumnFrame* out, std::string* error);

// a frame as plain arrays (plots: millions of points without a Value per point): numbers (int64, booleans as 0 / 1;
// a missing value is NaN) or texts per column
struct RawFrame {
  std::string table;
  FrameOp op = FrameOp::Replace;
  uint32_t nrows = 0;
  int64_t sent_ns = 0;
  std::vector<std::string> names;
  std::vector<std::vector<double>> num;           // [column] (empty for text columns)
  std::vector<std::vector<std::string>> text;     // [column] (empty for number columns)
  const std::vector<double>* Num(std::string_view name) const {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name && !num[i].empty()) return &num[i];
    }
    return nullptr;
  }
  const std::vector<std::string>* Text(std::string_view name) const {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name && !text[i].empty()) return &text[i];
    }
    return nullptr;
  }
};
bool DecodeRawFrame(const uint8_t* data, size_t size, RawFrame* out, std::string* error);

// encodes a frame (tests, recordings): a column of numbers -> Float64, booleans -> Bool, anything with text -> Utf8
std::vector<uint8_t> EncodeColumnFrame(const ColumnFrame& f);

// the text of a key value (rows are found by it): numbers in their shortest exact form, text as is
std::string BindKeyText(const core::Value& v);

// QueryPerformanceCounter now, in ns (the same clock as Python's time.perf_counter_ns())
int64_t PerfCounterNs();

// per-row update times of the bound tables (by table, then by body row), written by the engine and read by the UI
// without locks (the UI tolerates seeing a row's time one publish early)
struct BindTimes {
  explicit BindTimes(size_t rows = 0) : t(rows) {}
  std::vector<std::atomic<int64_t>> t;  // PerfCounterNs of the last update; 0 = never
  // columns with a change flash or a refresh cap: per row, when the shown value last changed and which way
  struct Col {
    uint32_t col = 0;  // the table column (0-based)
    std::string name;  // its name (the column can move)
    int flash = 0, flash_ms = 600, max_hz = 0;
    uint32_t up = 0, down = 0;  // ARGB
    std::vector<std::atomic<int64_t>> t;
    std::vector<std::atomic<int8_t>> dir;  // 1 up, -1 down, 0 other change
    explicit Col(size_t rows = 0) : t(rows), dir(rows) {}
  };
  std::vector<std::unique_ptr<Col>> cols;
  const Col* ColAt(uint32_t col) const {
    for (const auto& c : cols) {
      if (c->col == col) return c.get();
    }
    return nullptr;
  }
};

}  // namespace yc::engine
