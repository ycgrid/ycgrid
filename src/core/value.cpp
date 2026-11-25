// SPDX-License-Identifier: Apache-2.0
#include "core/value.h"

#include <cmath>
#include <cstdlib>
#include <new>

namespace yc::core {

const char* ErrorText(Err e) {
  switch (e) {
    case Err::Null: return "#NULL!";
    case Err::Div0: return "#DIV/0!";
    case Err::Value: return "#VALUE!";
    case Err::Ref: return "#REF!";
    case Err::Name: return "#NAME?";
    case Err::Num: return "#NUM!";
    case Err::NA: return "#N/A";
    case Err::GettingData: return "#GETTING_DATA";
    case Err::Spill: return "#SPILL!";
    case Err::Calc: return "#CALC!";
    case Err::Python: return "#PYTHON!";
    case Err::Connect: return "#CONNECT!";
    case Err::Blocked: return "#BLOCKED!";
    default: return "#ERR";
  }
}

bool ParseErrorText(std::string_view s, Err* out) {
  static const struct {
    const char* t;
    Err e;
  } kMap[] = {{"#NULL!", Err::Null}, {"#DIV/0!", Err::Div0},       {"#VALUE!", Err::Value}, {"#REF!", Err::Ref},
              {"#NAME?", Err::Name}, {"#NUM!", Err::Num},          {"#N/A", Err::NA},       {"#GETTING_DATA", Err::GettingData},
              {"#SPILL!", Err::Spill}, {"#CALC!", Err::Calc},      {"#PYTHON!", Err::Python}, {"#CONNECT!", Err::Connect},
              {"#BLOCKED!", Err::Blocked}};
  for (const auto& m : kMap) {
    if (s.size() == std::strlen(m.t)) {
      bool eq = true;
      for (size_t i = 0; i < s.size(); ++i) {
        char a = s[i], b = m.t[i];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
        if (a != b) {
          eq = false;
          break;
        }
      }
      if (eq) {
        *out = m.e;
        return true;
      }
    }
  }
  return false;
}

StrObj* StrObj::Make(std::string_view s) {
  void* mem = std::malloc(sizeof(StrObj) + s.size());
  if (!mem) throw std::bad_alloc();
  auto* p = static_cast<StrObj*>(mem);
  new (&p->rc) std::atomic<uint32_t>(1);
  p->len = static_cast<uint32_t>(s.size());
  std::memcpy(p->data, s.data(), s.size());
  p->data[s.size()] = 0;
  return p;
}

void StrObj::Release() {
  if (rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    rc.~atomic();
    std::free(this);
  }
}

Value Value::Number(double d) {
  if (!std::isfinite(d)) return Error(Err::Num);
  Value v;
  v.raw_[0] = static_cast<unsigned char>(VType::Number);
  if (d == 0.0) d = 0.0;  // -0 -> 0
  std::memcpy(v.raw_ + 8, &d, 8);
  return v;
}

Value Value::Bool(bool b) {
  Value v;
  v.raw_[0] = static_cast<unsigned char>(VType::Bool);
  v.raw_[1] = b ? 1 : 0;
  return v;
}

Value Value::Error(Err e) {
  Value v;
  v.raw_[0] = static_cast<unsigned char>(VType::Error);
  v.raw_[1] = static_cast<unsigned char>(e);
  return v;
}

Value Value::Pending() {
  Value v;
  v.raw_[0] = static_cast<unsigned char>(VType::Pending);
  return v;
}

Value Value::String(std::string_view s) {
  Value v;
  if (s.size() <= kInlineMax) {
    v.raw_[0] = static_cast<unsigned char>(VType::StrInline);
    v.raw_[1] = static_cast<unsigned char>(s.size());
    std::memcpy(v.raw_ + 2, s.data(), s.size());
  } else {
    v.raw_[0] = static_cast<unsigned char>(VType::StrHeap);
    StrObj* p = StrObj::Make(s);
    std::memcpy(v.raw_ + 8, &p, sizeof(p));
  }
  return v;
}

Value Value::DecimalRaw(const Int128& m, int scale) {
  Value v;
  v.raw_[0] = static_cast<unsigned char>(VType::Decimal);
  v.raw_[1] = static_cast<unsigned char>(scale);
  // 112-bit mantissa: 8 bytes of lo + the low 6 bytes of hi
  std::memcpy(v.raw_ + 2, &m.lo, 8);
  std::memcpy(v.raw_ + 10, &m.hi, 6);
  return v;
}

Int128 Value::dec_mantissa() const {
  Int128 m;
  std::memcpy(&m.lo, raw_ + 2, 8);
  int64_t hi = 0;
  std::memcpy(&hi, raw_ + 10, 6);
  // sign-extend from 48 bits
  if (hi & (int64_t{1} << 47)) hi |= ~((int64_t{1} << 48) - 1);
  m.hi = hi;
  return m;
}

std::string_view Value::str() const {
  if (type() == VType::StrInline) return std::string_view(reinterpret_cast<const char*>(raw_ + 2), raw_[1]);
  if (type() == VType::StrHeap) return heap()->view();
  return {};
}

bool Value::SameAs(const Value& o) const {
  if (IsString() && o.IsString()) return str() == o.str();
  return std::memcmp(raw_, o.raw_, 16) == 0;
}

}  // namespace yc::core
