// SPDX-License-Identifier: Apache-2.0
// tape.h - [YC] recordings of bound-table data (.yctape): record a session's frames, replay them later
//
// A tape is the column frames (bind.h) that reached YCGrid, each with the time it arrived. Replaying feeds them to the
// same bound tables at their recorded pace (or faster), so a trading screen runs on recorded data unchanged.
//
//   header (32 bytes): "YCTAPE\0\1" | uint32 version | uint32 header size | int64 start (Unix ms) | int64 reserved
//   records: int64 t (ns since the start) | uint32 length | uint32 kind | frame bytes, padded to 8 bytes
//
// A recording starts with a snapshot of every bound table (Replace frames at t = 0), so replaying from any point
// rebuilds the tables from that snapshot plus the frames before the point.
#pragma once

#include "core/value.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace yc::engine {

inline constexpr char kTapeMagic[8] = {'Y', 'C', 'T', 'A', 'P', 'E', 0, 1};
inline constexpr uint32_t kTapeVersion = 1;
inline constexpr uint32_t kTapeHeader = 32;
inline constexpr uint32_t kTapeFrame = 1;   // record kind: a column frame
inline constexpr uint32_t kTapeTopics = 2;  // record kind: a batch of topic updates (Topics.Push, =QUOTE())
inline constexpr uint32_t kTapePlot = 3;    // record kind: a figure request (yc.plot), encoded by the app
inline constexpr uint32_t kTapeMark = 4;    // record kind: a marker on the timeline, UTF-8 "kind\ttext"

// a batch of topic updates as bytes: uint32 count, then per item uint16 name length, name, uint8 type
// (0 empty, 1 number, 2 text, 3 bool, 4 error) and the value (double, uint32 length + UTF-8, uint8)
std::vector<uint8_t> EncodeTopicBatch(const std::vector<std::pair<std::string, core::Value>>& items);
bool DecodeTopicBatch(const uint8_t* data, size_t size, std::vector<std::pair<std::string, core::Value>>* out);

class TapeWriter {
 public:
  TapeWriter() = default;
  TapeWriter(const TapeWriter&) = delete;
  TapeWriter& operator=(const TapeWriter&) = delete;
  ~TapeWriter() { Close(); }
  bool Open(const std::wstring& path, int64_t start_unix_ms, std::string* error);
  bool Write(int64_t t_ns, const uint8_t* data, size_t len, uint32_t kind = kTapeFrame);
  void Close();
  bool IsOpen() const { return f_ != nullptr; }
  uint64_t frames() const { return frames_; }
  uint64_t bytes() const { return bytes_; }
  // what was written reaches the file at least this often (and the header at once): a YCGrid that crashes or is
  // ended mid-recording keeps all but the last moments, where the 1 MB buffer kept even the header from the file
  int64_t flush_every_ns = 1'000'000'000;

 private:
  std::FILE* f_ = nullptr;
  uint64_t frames_ = 0, bytes_ = 0;
  int64_t last_flush_ns_ = 0;
};

// where each frame of a tape is (read once when a replay starts)
struct TapeIndex {
  int64_t start_unix_ms = 0;
  std::vector<int64_t> t;      // ns since the start, ascending
  std::vector<uint64_t> off;   // the frame bytes' file offsets
  std::vector<uint32_t> len;
  std::vector<uint16_t> table;     // per record: its table (an index into tables); kTopicRecord for topic batches
  std::vector<std::string> tables;  // the tables on the tape
  std::vector<std::string> topics;  // the topics on the tape (their live updates are ignored while it plays)
  static constexpr uint16_t kTopicRecord = 0xFFFF;
  static constexpr uint16_t kPlotRecord = 0xFFFE;  // a figure request: replayed by the app
  struct Mark {
    int64_t t = 0;  // ns since the start
    std::string kind, text;
  };
  std::vector<Mark> marks;  // events to show on the timeline (alerts, fills, notes)
  int64_t length_ns() const { return t.empty() ? 0 : t.back(); }
};

class TapeReader {
 public:
  TapeReader() = default;
  TapeReader(const TapeReader&) = delete;
  TapeReader& operator=(const TapeReader&) = delete;
  ~TapeReader() { Close(); }
  // opens the tape and indexes its frames; a tape cut short (a crash while recording) keeps its whole frames
  bool Open(const std::wstring& path, std::string* error);
  bool Frame(size_t i, std::vector<uint8_t>* out);
  void Close();
  const TapeIndex& index() const { return idx_; }

 private:
  std::FILE* f_ = nullptr;
  TapeIndex idx_;
};

}  // namespace yc::engine
