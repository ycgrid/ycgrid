// SPDX-License-Identifier: Apache-2.0
// msgvalue.h - a simple msgpack value tree (YCP request parameters and replies)
#pragma once

#include "io/msgpack.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yc::io {

struct MV {
  enum class T : uint8_t { Nil, Bool, Int, Double, Str, Bin, Ext, Array, Map } t = T::Nil;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;  // contents of Str, Bin, Ext
  int8_t ext = 0;
  std::vector<MV> a;
  std::vector<std::pair<std::string, MV>> m;

  static MV Nil() { return MV{}; }
  static MV Bool(bool v) {
    MV x;
    x.t = T::Bool;
    x.b = v;
    return x;
  }
  static MV Int(int64_t v) {
    MV x;
    x.t = T::Int;
    x.i = v;
    return x;
  }
  static MV Double(double v) {
    MV x;
    x.t = T::Double;
    x.d = v;
    return x;
  }
  static MV Str(std::string v) {
    MV x;
    x.t = T::Str;
    x.s = std::move(v);
    return x;
  }
  static MV Bin(std::string v) {
    MV x;
    x.t = T::Bin;
    x.s = std::move(v);
    return x;
  }
  static MV Ext(int8_t type, std::string v) {
    MV x;
    x.t = T::Ext;
    x.ext = type;
    x.s = std::move(v);
    return x;
  }
  static MV Array() {
    MV x;
    x.t = T::Array;
    return x;
  }
  static MV Map() {
    MV x;
    x.t = T::Map;
    return x;
  }

  bool IsNil() const { return t == T::Nil; }
  bool IsNum() const { return t == T::Int || t == T::Double; }
  MV& Push(MV v) {
    a.push_back(std::move(v));
    return a.back();
  }
  MV& Set(std::string k, MV v) {
    for (auto& kv : m) {
      if (kv.first == k) {
        kv.second = std::move(v);
        return kv.second;
      }
    }
    m.emplace_back(std::move(k), std::move(v));
    return m.back().second;
  }
  const MV* Get(std::string_view k) const {
    for (const auto& kv : m) {
      if (kv.first == k) return &kv.second;
    }
    return nullptr;
  }
  // convenience accessors (return the default when missing or of the wrong type)
  int64_t GetInt(std::string_view k, int64_t def = 0) const {
    const MV* v = Get(k);
    if (!v) return def;
    if (v->t == T::Int) return v->i;
    if (v->t == T::Double) return static_cast<int64_t>(v->d);
    if (v->t == T::Bool) return v->b ? 1 : 0;
    return def;
  }
  double GetDouble(std::string_view k, double def = 0) const {
    const MV* v = Get(k);
    if (!v) return def;
    if (v->t == T::Int) return static_cast<double>(v->i);
    if (v->t == T::Double) return v->d;
    return def;
  }
  bool GetBool(std::string_view k, bool def = false) const {
    const MV* v = Get(k);
    if (!v) return def;
    if (v->t == T::Bool) return v->b;
    if (v->t == T::Int) return v->i != 0;
    return def;
  }
  std::string GetStr(std::string_view k, std::string def = {}) const {
    const MV* v = Get(k);
    return v && (v->t == T::Str || v->t == T::Bin) ? v->s : def;
  }
  bool Has(std::string_view k) const {
    const MV* v = Get(k);
    return v && !v->IsNil();
  }
  double Num() const { return t == T::Int ? static_cast<double>(i) : (t == T::Double ? d : 0.0); }
};

inline void Encode(MsgWriter& w, const MV& v) {
  switch (v.t) {
    case MV::T::Nil: w.Nil(); break;
    case MV::T::Bool: w.Bool(v.b); break;
    case MV::T::Int: w.Int(v.i); break;
    case MV::T::Double: w.Double(v.d); break;
    case MV::T::Str: w.Str(v.s); break;
    case MV::T::Bin: w.Bin(v.s); break;
    case MV::T::Ext: w.Ext(v.ext, v.s); break;
    case MV::T::Array:
      w.Array(static_cast<uint32_t>(v.a.size()));
      for (const MV& x : v.a) Encode(w, x);
      break;
    case MV::T::Map:
      w.Map(static_cast<uint32_t>(v.m.size()));
      for (const auto& [k, x] : v.m) {
        w.Str(k);
        Encode(w, x);
      }
      break;
  }
}

inline bool Decode(MsgReader& r, MV* out, int depth = 0) {
  if (depth > 64) return false;
  switch (r.Peek()) {
    case MsgType::Nil: *out = MV::Nil(); return r.Nil();
    case MsgType::Bool: {
      out->t = MV::T::Bool;
      return r.Bool(&out->b);
    }
    case MsgType::Int:
    case MsgType::UInt: {
      out->t = MV::T::Int;
      if (r.Int(&out->i)) return true;
      uint64_t u;
      if (!r.UInt(&u)) return false;
      out->t = MV::T::Double;
      out->d = static_cast<double>(u);
      return true;
    }
    case MsgType::Float: out->t = MV::T::Double; return r.Double(&out->d);
    case MsgType::Str: out->t = MV::T::Str; return r.Str(&out->s);
    case MsgType::Bin: out->t = MV::T::Bin; return r.Bin(&out->s);
    case MsgType::Ext: {
      out->t = MV::T::Ext;
      std::string_view d;
      if (!r.Ext(&out->ext, &d)) return false;
      out->s.assign(d);
      return true;
    }
    case MsgType::Array: {
      uint32_t n;
      if (!r.Array(&n)) return false;
      out->t = MV::T::Array;
      out->a.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        if (!Decode(r, &out->a[i], depth + 1)) return false;
      }
      return true;
    }
    case MsgType::Map: {
      uint32_t n;
      if (!r.Map(&n)) return false;
      out->t = MV::T::Map;
      out->m.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        MV k, v;
        if (!Decode(r, &k, depth + 1) || !Decode(r, &v, depth + 1)) return false;
        std::string key = k.t == MV::T::Str ? k.s : (k.t == MV::T::Int ? std::to_string(k.i) : std::string());
        out->m.emplace_back(std::move(key), std::move(v));
      }
      return true;
    }
    default: return false;
  }
}

}  // namespace yc::io
