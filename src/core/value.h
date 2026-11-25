// SPDX-License-Identifier: Apache-2.0
// value.h - cell value (PLAN §5.2, §5.2a): 16 bytes
//
// byte 0      type (VType)
// byte 1      sub info: error code / bool / inline string length / Decimal scale
// bytes 2..15 inline string (UTF-8, up to 14 bytes) or the Decimal's 112-bit mantissa
// bytes 8..15 double or StrObj* (heap string, reference counted)
//
// Value has reference-counting semantics (copies AddRef the heap string), so when tiles store Value arrays,
// copying a tile (COW) shares the strings correctly. Strings are immutable and can be shared across threads.
#pragma once

#include "core/int128.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace yc::core {

enum class VType : uint8_t {
  Empty = 0,
  Number = 1,
  Bool = 2,
  Error = 3,
  StrInline = 4,
  StrHeap = 5,
  Decimal = 6,
  Pending = 7,  // waiting for an asynchronous result (UDF / stream), shown as #GETTING_DATA
};

enum class Err : uint8_t {
  None = 0,
  Null = 1,
  Div0,
  Value,
  Ref,
  Name,
  Num,
  NA,
  GettingData,
  Spill,
  Calc,
  Python,
  Connect,
  Blocked,  // #BLOCKED!: Python code that the Trust Center keeps from running (=PY in a workbook whose code is disabled)
};

const char* ErrorText(Err e);
bool ParseErrorText(std::string_view s, Err* out);

struct StrObj {
  std::atomic<uint32_t> rc;
  uint32_t len;
  char data[1];

  static StrObj* Make(std::string_view s);
  void AddRef() { rc.fetch_add(1, std::memory_order_relaxed); }
  void Release();
  std::string_view view() const { return std::string_view(data, len); }
};

class Value {
 public:
  static constexpr size_t kInlineMax = 14;
  static constexpr int kDecimalMaxScale = 28;

  Value() { std::memset(raw_, 0, sizeof(raw_)); }
  Value(const Value& o) {
    std::memcpy(raw_, o.raw_, sizeof(raw_));
    if (type() == VType::StrHeap) heap()->AddRef();
  }
  Value(Value&& o) noexcept {
    std::memcpy(raw_, o.raw_, sizeof(raw_));
    std::memset(o.raw_, 0, sizeof(o.raw_));
  }
  Value& operator=(const Value& o) {
    if (this != &o) {
      Value tmp(o);
      Swap(tmp);
    }
    return *this;
  }
  Value& operator=(Value&& o) noexcept {
    if (this != &o) {
      Reset();
      std::memcpy(raw_, o.raw_, sizeof(raw_));
      std::memset(o.raw_, 0, sizeof(o.raw_));
    }
    return *this;
  }
  ~Value() { Reset(); }

  void Swap(Value& o) noexcept {
    alignas(16) unsigned char t[16];
    std::memcpy(t, raw_, 16);
    std::memcpy(raw_, o.raw_, 16);
    std::memcpy(o.raw_, t, 16);
  }
  void Reset() {
    if (type() == VType::StrHeap) heap()->Release();
    std::memset(raw_, 0, sizeof(raw_));
  }

  // ---- Construction ----
  static Value Number(double d);           // NaN / Inf become #NUM!
  static Value Bool(bool b);
  static Value Error(Err e);
  static Value String(std::string_view s);
  static Value DecimalRaw(const Int128& mantissa, int scale);  // not normalized
  static Value Pending();

  // ---- Queries ----
  VType type() const { return static_cast<VType>(raw_[0]); }
  bool IsEmpty() const { return type() == VType::Empty; }
  bool IsNumber() const { return type() == VType::Number; }
  bool IsBool() const { return type() == VType::Bool; }
  bool IsError() const { return type() == VType::Error; }
  bool IsString() const { return type() == VType::StrInline || type() == VType::StrHeap; }
  bool IsDecimal() const { return type() == VType::Decimal; }
  bool IsNumeric() const { return IsNumber() || IsDecimal(); }
  bool IsPending() const { return type() == VType::Pending; }

  double number() const {
    double d;
    std::memcpy(&d, raw_ + 8, 8);
    return d;
  }
  bool boolean() const { return raw_[1] != 0; }
  Err error() const { return static_cast<Err>(raw_[1]); }
  std::string_view str() const;
  Int128 dec_mantissa() const;
  int dec_scale() const { return raw_[1]; }

  // numeric (Number or Decimal) to double
  double AsDouble() const { return IsDecimal() ? dec_mantissa().ToDouble() / Pow10(dec_scale()).ToDouble() : number(); }

  // bitwise identical (for early exit): strings compare contents
  bool SameAs(const Value& o) const;
  // the same value to the bit (SameAs), so structs that hold values compare with a defaulted ==
  friend bool operator==(const Value& a, const Value& b) { return a.SameAs(b); }

  // Block copies of many values (a sheet tile): copy the bytes, then call this on each copy so a shared string is
  // counted once more.
  bool IsHeapString() const { return type() == VType::StrHeap; }
  void RetainAfterByteCopy() const {
    if (type() == VType::StrHeap) heap()->AddRef();
  }

 private:
  StrObj* heap() const {
    StrObj* p;
    std::memcpy(&p, raw_ + 8, sizeof(p));
    return p;
  }

  alignas(16) unsigned char raw_[16];
};

static_assert(sizeof(Value) == 16, "Value must be 16 bytes");

}  // namespace yc::core
