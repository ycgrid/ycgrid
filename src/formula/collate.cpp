// SPDX-License-Identifier: Apache-2.0
// collate.cpp - the order Excel gives texts: Windows' word sort for the user's locale, as Excel uses
#include "formula/collate.h"

#include <windows.h>

#include <string>

namespace yc::formula {

namespace {

// letters, digits and spaces only: Windows' word sort puts space < digits < letters, which is their order in ASCII,
// so the bytes decide (the common case: tickers, names, codes)
bool Plain(std::string_view s) {
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!(c == ' ' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  }
  return true;
}

std::wstring Wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

int Bytes(std::string_view a, std::string_view b) {
  const int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

}  // namespace

int CollateLowered(std::string_view a, std::string_view b) {
  if (a == b) return 0;
  if (Plain(a) && Plain(b)) return Bytes(a, b);
  const std::wstring x = Wide(a), y = Wide(b);
  const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, x.c_str(), static_cast<int>(x.size()), y.c_str(),
                                static_cast<int>(y.size()), nullptr, nullptr, 0);
  if (r == CSTR_LESS_THAN) return -1;
  if (r == CSTR_GREATER_THAN) return 1;
  return Bytes(a, b);  // the same to Windows (or no answer) but different texts: still a fixed order
}

int CollateCased(std::string_view a, std::string_view b) {
  if (a == b) return 0;
  const std::wstring x = Wide(a), y = Wide(b);
  const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, 0, x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()),
                                nullptr, nullptr, 0);
  if (r == CSTR_LESS_THAN) return -1;
  if (r == CSTR_GREATER_THAN) return 1;
  return Bytes(a, b);
}

}  // namespace yc::formula
