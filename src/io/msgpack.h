// SPDX-License-Identifier: Apache-2.0
// msgpack.h - minimal MessagePack reader/writer (shared by .ycg metadata chunks, KV and the YCP protocol; PLAN §15.4)
//
// Writing: MsgWriter appends to a std::string. Reading: MsgReader parses with a cursor; any format error returns false
// (no exceptions), and unknown fields can be skipped with Skip() - the basis of "fields are only added, readers ignore unknown fields".
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yc::io {

class MsgWriter {
 public:
  std::string& buf() { return b_; }
  const std::string& buf() const { return b_; }

  void Nil() { b_ += '\xc0'; }
  void Bool(bool v) { b_ += v ? '\xc3' : '\xc2'; }
  void Int(int64_t v);
  void UInt(uint64_t v);
  void Double(double v);
  void Str(std::string_view s);
  void Bin(std::string_view s);
  void Array(uint32_t n);
  void Map(uint32_t n);
  void Ext(int8_t type, std::string_view data);
  // write already-encoded msgpack as is (KV values)
  void Raw(std::string_view encoded) { b_.append(encoded); }

  // helpers for integer-keyed maps
  void KInt(int key, int64_t v) { Int(key), Int(v); }
  void KStr(int key, std::string_view v) { Int(key), Str(v); }
  void KBool(int key, bool v) { Int(key), Bool(v); }
  void KDouble(int key, double v) { Int(key), Double(v); }

 private:
  void Be(uint64_t v, int bytes);
  std::string b_;
};

enum class MsgType : uint8_t { Nil, Bool, Int, UInt, Float, Str, Bin, Array, Map, Ext, Invalid };

class MsgReader {
 public:
  explicit MsgReader(std::string_view data) : d_(data) {}
  bool AtEnd() const { return p_ >= d_.size(); }
  size_t pos() const { return p_; }

  MsgType Peek() const;
  bool Nil();
  bool Bool(bool* out);
  bool Int(int64_t* out);  // accepts signed and unsigned (within range)
  bool UInt(uint64_t* out);
  bool Double(double* out);  // accepts integers and float32/64
  bool Str(std::string* out);
  bool StrView(std::string_view* out);
  bool Bin(std::string* out);  // accepts str too
  bool Array(uint32_t* n);
  bool Map(uint32_t* n);
  bool Ext(int8_t* type, std::string_view* data);
  bool Skip(int depth = 0);  // skip any one value (nested at most 64 deep: a damaged file can't overflow the stack)
  // the raw encoding of one complete value (KV is stored as is)
  bool RawValue(std::string_view* out);

  // helper: read a number and convert it
  template <class T>
  bool IntAs(T* out) {
    int64_t v;
    if (!Int(&v)) return false;
    *out = static_cast<T>(v);
    return true;
  }

 private:
  bool Need(size_t n) const { return p_ + n <= d_.size(); }
  uint64_t ReadBe(int bytes);
  std::string_view d_;
  size_t p_ = 0;
};

// crc32c (Castagnoli)
uint32_t Crc32c(const void* data, size_t n, uint32_t crc = 0);

}  // namespace yc::io
