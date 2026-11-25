// SPDX-License-Identifier: Apache-2.0
#include "io/msgpack.h"

#include <array>
#include <cstring>

namespace yc::io {

void MsgWriter::Be(uint64_t v, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) b_ += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void MsgWriter::UInt(uint64_t v) {
  if (v < 0x80) {
    b_ += static_cast<char>(v);
  } else if (v <= 0xFF) {
    b_ += '\xcc';
    Be(v, 1);
  } else if (v <= 0xFFFF) {
    b_ += '\xcd';
    Be(v, 2);
  } else if (v <= 0xFFFFFFFFull) {
    b_ += '\xce';
    Be(v, 4);
  } else {
    b_ += '\xcf';
    Be(v, 8);
  }
}

void MsgWriter::Int(int64_t v) {
  if (v >= 0) {
    UInt(static_cast<uint64_t>(v));
    return;
  }
  if (v >= -32) {
    b_ += static_cast<char>(static_cast<int8_t>(v));
  } else if (v >= INT8_MIN) {
    b_ += '\xd0';
    Be(static_cast<uint8_t>(v), 1);
  } else if (v >= INT16_MIN) {
    b_ += '\xd1';
    Be(static_cast<uint16_t>(v), 2);
  } else if (v >= INT32_MIN) {
    b_ += '\xd2';
    Be(static_cast<uint32_t>(v), 4);
  } else {
    b_ += '\xd3';
    Be(static_cast<uint64_t>(v), 8);
  }
}

void MsgWriter::Double(double v) {
  b_ += '\xcb';
  uint64_t u;
  std::memcpy(&u, &v, 8);
  Be(u, 8);
}

void MsgWriter::Str(std::string_view s) {
  const size_t n = s.size();
  if (n < 32) {
    b_ += static_cast<char>(0xa0 | n);
  } else if (n <= 0xFF) {
    b_ += '\xd9';
    Be(n, 1);
  } else if (n <= 0xFFFF) {
    b_ += '\xda';
    Be(n, 2);
  } else {
    b_ += '\xdb';
    Be(n, 4);
  }
  b_.append(s);
}

void MsgWriter::Bin(std::string_view s) {
  const size_t n = s.size();
  if (n <= 0xFF) {
    b_ += '\xc4';
    Be(n, 1);
  } else if (n <= 0xFFFF) {
    b_ += '\xc5';
    Be(n, 2);
  } else {
    b_ += '\xc6';
    Be(n, 4);
  }
  b_.append(s);
}

void MsgWriter::Array(uint32_t n) {
  if (n < 16) {
    b_ += static_cast<char>(0x90 | n);
  } else if (n <= 0xFFFF) {
    b_ += '\xdc';
    Be(n, 2);
  } else {
    b_ += '\xdd';
    Be(n, 4);
  }
}

void MsgWriter::Map(uint32_t n) {
  if (n < 16) {
    b_ += static_cast<char>(0x80 | n);
  } else if (n <= 0xFFFF) {
    b_ += '\xde';
    Be(n, 2);
  } else {
    b_ += '\xdf';
    Be(n, 4);
  }
}

void MsgWriter::Ext(int8_t type, std::string_view data) {
  const size_t n = data.size();
  switch (n) {
    case 1: b_ += '\xd4'; break;
    case 2: b_ += '\xd5'; break;
    case 4: b_ += '\xd6'; break;
    case 8: b_ += '\xd7'; break;
    case 16: b_ += '\xd8'; break;
    default:
      if (n <= 0xFF) {
        b_ += '\xc7';
        Be(n, 1);
      } else if (n <= 0xFFFF) {
        b_ += '\xc8';
        Be(n, 2);
      } else {
        b_ += '\xc9';
        Be(n, 4);
      }
      break;
  }
  b_ += static_cast<char>(type);
  b_.append(data);
}

// ---- Reader ----

uint64_t MsgReader::ReadBe(int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | static_cast<uint8_t>(d_[p_++]);
  return v;
}

MsgType MsgReader::Peek() const {
  if (AtEnd()) return MsgType::Invalid;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  if (t <= 0x7f || (t >= 0xcc && t <= 0xcf)) return MsgType::UInt;
  if (t >= 0xe0 || (t >= 0xd0 && t <= 0xd3)) return MsgType::Int;
  if ((t & 0xf0) == 0x80 || t == 0xde || t == 0xdf) return MsgType::Map;
  if ((t & 0xf0) == 0x90 || t == 0xdc || t == 0xdd) return MsgType::Array;
  if ((t & 0xe0) == 0xa0 || (t >= 0xd9 && t <= 0xdb)) return MsgType::Str;
  if (t >= 0xc4 && t <= 0xc6) return MsgType::Bin;
  if (t == 0xc0) return MsgType::Nil;
  if (t == 0xc2 || t == 0xc3) return MsgType::Bool;
  if (t == 0xca || t == 0xcb) return MsgType::Float;
  if ((t >= 0xc7 && t <= 0xc9) || (t >= 0xd4 && t <= 0xd8)) return MsgType::Ext;
  return MsgType::Invalid;
}

bool MsgReader::Nil() {
  if (!Need(1) || static_cast<uint8_t>(d_[p_]) != 0xc0) return false;
  ++p_;
  return true;
}

bool MsgReader::Bool(bool* out) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  if (t != 0xc2 && t != 0xc3) return false;
  *out = t == 0xc3;
  ++p_;
  return true;
}

bool MsgReader::Int(int64_t* out) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  if (t <= 0x7f) {
    *out = t;
    ++p_;
    return true;
  }
  if (t >= 0xe0) {
    *out = static_cast<int8_t>(t);
    ++p_;
    return true;
  }
  int bytes = 0;
  bool sign = false;
  switch (t) {
    case 0xcc: bytes = 1; break;
    case 0xcd: bytes = 2; break;
    case 0xce: bytes = 4; break;
    case 0xcf: bytes = 8; break;
    case 0xd0: bytes = 1; sign = true; break;
    case 0xd1: bytes = 2; sign = true; break;
    case 0xd2: bytes = 4; sign = true; break;
    case 0xd3: bytes = 8; sign = true; break;
    default: return false;
  }
  if (!Need(1 + static_cast<size_t>(bytes))) return false;
  ++p_;
  const uint64_t u = ReadBe(bytes);
  if (sign) {
    switch (bytes) {
      case 1: *out = static_cast<int8_t>(u); break;
      case 2: *out = static_cast<int16_t>(u); break;
      case 4: *out = static_cast<int32_t>(u); break;
      default: *out = static_cast<int64_t>(u); break;
    }
  } else {
    if (bytes == 8 && u > static_cast<uint64_t>(INT64_MAX)) {
      p_ -= 9;  // refused: the value stays for UInt (a failed read leaves the cursor where it was)
      return false;
    }
    *out = static_cast<int64_t>(u);
  }
  return true;
}

bool MsgReader::UInt(uint64_t* out) {
  const size_t save = p_;
  if (Need(1) && static_cast<uint8_t>(d_[p_]) == 0xcf) {
    if (!Need(9)) return false;
    ++p_;
    *out = ReadBe(8);
    return true;
  }
  int64_t v;
  if (!Int(&v) || v < 0) {
    p_ = save;
    return false;
  }
  *out = static_cast<uint64_t>(v);
  return true;
}

bool MsgReader::Double(double* out) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  if (t == 0xcb) {
    if (!Need(9)) return false;
    ++p_;
    const uint64_t u = ReadBe(8);
    std::memcpy(out, &u, 8);
    return true;
  }
  if (t == 0xca) {
    if (!Need(5)) return false;
    ++p_;
    const uint32_t u = static_cast<uint32_t>(ReadBe(4));
    float f;
    std::memcpy(&f, &u, 4);
    *out = f;
    return true;
  }
  int64_t i;
  if (Int(&i)) {
    *out = static_cast<double>(i);
    return true;
  }
  return false;
}

bool MsgReader::StrView(std::string_view* out) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  size_t n = 0;
  size_t hdr = 1;
  if ((t & 0xe0) == 0xa0) n = t & 0x1f;
  else if (t == 0xd9 || t == 0xc4) hdr = 2;
  else if (t == 0xda || t == 0xc5) hdr = 3;
  else if (t == 0xdb || t == 0xc6) hdr = 5;
  else return false;
  if (!Need(hdr)) return false;
  const size_t start = p_;
  ++p_;
  if (hdr > 1) n = static_cast<size_t>(ReadBe(static_cast<int>(hdr - 1)));
  if (!Need(n)) {
    p_ = start;  // cut off: nothing read
    return false;
  }
  *out = d_.substr(p_, n);
  p_ += n;
  return true;
}

bool MsgReader::Str(std::string* out) {
  std::string_view v;
  if (!StrView(&v)) return false;
  out->assign(v);
  return true;
}

bool MsgReader::Bin(std::string* out) { return Str(out); }

bool MsgReader::Array(uint32_t* n) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  if ((t & 0xf0) == 0x90) {
    *n = t & 0x0f;
    ++p_;
    return true;
  }
  const int bytes = t == 0xdc ? 2 : (t == 0xdd ? 4 : 0);
  if (!bytes || !Need(1 + static_cast<size_t>(bytes))) return false;
  ++p_;
  *n = static_cast<uint32_t>(ReadBe(bytes));
  return true;
}

bool MsgReader::Map(uint32_t* n) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  if ((t & 0xf0) == 0x80) {
    *n = t & 0x0f;
    ++p_;
    return true;
  }
  const int bytes = t == 0xde ? 2 : (t == 0xdf ? 4 : 0);
  if (!bytes || !Need(1 + static_cast<size_t>(bytes))) return false;
  ++p_;
  *n = static_cast<uint32_t>(ReadBe(bytes));
  return true;
}

bool MsgReader::Ext(int8_t* type, std::string_view* data) {
  if (!Need(1)) return false;
  const uint8_t t = static_cast<uint8_t>(d_[p_]);
  size_t len = 0, hdr = 2;
  switch (t) {
    case 0xd4: len = 1; break;
    case 0xd5: len = 2; break;
    case 0xd6: len = 4; break;
    case 0xd7: len = 8; break;
    case 0xd8: len = 16; break;
    case 0xc7: hdr = 3; break;
    case 0xc8: hdr = 4; break;
    case 0xc9: hdr = 6; break;
    default: return false;
  }
  if (!Need(hdr)) return false;
  ++p_;
  if (hdr > 2) len = static_cast<size_t>(ReadBe(static_cast<int>(hdr - 2)));
  *type = static_cast<int8_t>(d_[p_++]);
  if (!Need(len)) return false;
  *data = d_.substr(p_, len);
  p_ += len;
  return true;
}

bool MsgReader::Skip(int depth) {
  if (depth > 64) return false;
  switch (Peek()) {
    case MsgType::Nil: return Nil();
    case MsgType::Bool: {
      bool b;
      return Bool(&b);
    }
    case MsgType::Int:
    case MsgType::UInt: {
      uint64_t u;
      const size_t save = p_;
      if (UInt(&u)) return true;
      p_ = save;
      int64_t i;
      return Int(&i);
    }
    case MsgType::Float: {
      double d;
      return Double(&d);
    }
    case MsgType::Str:
    case MsgType::Bin: {
      std::string_view v;
      return StrView(&v);
    }
    case MsgType::Array: {
      uint32_t n;
      if (!Array(&n)) return false;
      for (uint32_t i = 0; i < n; ++i) {
        if (!Skip(depth + 1)) return false;
      }
      return true;
    }
    case MsgType::Map: {
      uint32_t n;
      if (!Map(&n)) return false;
      for (uint64_t i = 0; i < uint64_t{n} * 2; ++i) {
        if (!Skip(depth + 1)) return false;
      }
      return true;
    }
    case MsgType::Ext: {
      const uint8_t t = static_cast<uint8_t>(d_[p_]);
      size_t len = 0, hdr = 2;
      switch (t) {
        case 0xd4: len = 1; break;
        case 0xd5: len = 2; break;
        case 0xd6: len = 4; break;
        case 0xd7: len = 8; break;
        case 0xd8: len = 16; break;
        case 0xc7: hdr = 3; break;
        case 0xc8: hdr = 4; break;
        case 0xc9: hdr = 6; break;
        default: return false;
      }
      if (!Need(hdr)) return false;
      ++p_;
      if (hdr > 2) len = static_cast<size_t>(ReadBe(static_cast<int>(hdr - 2)));
      ++p_;  // type
      if (!Need(len)) return false;
      p_ += len;
      return true;
    }
    default: return false;
  }
}

bool MsgReader::RawValue(std::string_view* out) {
  const size_t start = p_;
  if (!Skip()) return false;
  *out = d_.substr(start, p_ - start);
  return true;
}

uint32_t Crc32c(const void* data, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace yc::io
