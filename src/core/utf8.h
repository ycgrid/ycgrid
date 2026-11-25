// SPDX-License-Identifier: Apache-2.0
// utf8.h - cutting and counting UTF-8 text by characters, never in the middle of one: a string cut by bytes
// (substr(0, n)) can end with half a Chinese character, which no reader decodes (the UI shows a broken glyph, and a
// client reading it over YCP fails on every later call).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace yc::core {

// true for a byte that starts a character (ASCII, or the lead byte of a sequence); continuation bytes are 10xxxxxx
inline bool Utf8Lead(unsigned char b) { return (b & 0xC0) != 0x80; }

// the number of characters (code points) in s
inline size_t Utf8Length(std::string_view s) {
  size_t n = 0;
  for (const char ch : s) n += Utf8Lead(static_cast<unsigned char>(ch)) ? 1 : 0;
  return n;
}

// the longest prefix of s that is at most max_bytes long and ends on a character boundary
inline std::string_view Utf8Prefix(std::string_view s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t n = max_bytes;
  while (n > 0 && !Utf8Lead(static_cast<unsigned char>(s[n]))) --n;
  return s.substr(0, n);
}

// the first max_chars characters of s
inline std::string_view Utf8First(std::string_view s, size_t max_chars) {
  size_t seen = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!Utf8Lead(static_cast<unsigned char>(s[i]))) continue;
    if (seen == max_chars) return s.substr(0, i);
    ++seen;
  }
  return s;
}

}  // namespace yc::core
