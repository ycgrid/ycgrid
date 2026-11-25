// SPDX-License-Identifier: Apache-2.0
#include "core/py_source.h"

namespace yc::core {

bool IsPythonName(std::string_view name) {
  if (name.empty() || (name[0] >= '0' && name[0] <= '9')) return false;
  for (const char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    const bool ok = c >= 0x80 || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!ok) return false;
  }
  return true;
}

namespace {

// `def <fn>(` (spaces allowed after def and before the parenthesis) starting at `p`
bool DefinesAt(std::string_view text, size_t p, std::string_view fn) {
  if (text.substr(p, 3) != "def") return false;
  p += 3;
  const size_t after_def = p;
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  if (p == after_def || text.substr(p, fn.size()) != fn) return false;
  p += fn.size();
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  return p < text.size() && text[p] == '(';
}

}  // namespace

HandlerPlace EnsurePythonHandler(std::string& text, std::string_view fn) {
  HandlerPlace at;
  // a UTF-8 byte order mark is not part of the first line
  const size_t start = text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  int line = 1;
  for (size_t p = start; p < text.size();) {
    if (DefinesAt(text, p, fn)) {
      // the line after `def`; a handler on the last line has no such line, so the caret goes to `def` itself
      at.line = text.find('\n', p) == std::string::npos ? line : line + 1;
      return at;
    }
    const size_t nl = text.find('\n', p);
    if (nl == std::string::npos) break;
    p = nl + 1;
    ++line;
  }
  const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  if (text.size() > start && text.back() != '\n') text += eol;
  // the blank lines already at the end count toward the two PEP 8 asks for before a function
  const std::string_view all(text);
  int blank = 0;
  for (size_t e = text.size(); blank < 2 && e > start;) {  // text[e - 1] ends the line looked at
    const size_t prev = e - 1 > start ? all.rfind('\n', e - 2) : std::string_view::npos;
    const size_t b = prev == std::string_view::npos || prev < start ? start : prev + 1;
    if (all.substr(b, e - b).find_first_not_of(" \t\r\n") != std::string_view::npos) break;
    ++blank;
    e = b;
  }
  const bool empty = all.find_first_not_of(" \t\r\n", start) == std::string_view::npos;
  if (!empty) {
    for (; blank < 2; ++blank) text += eol;
  }
  int lines = 1;
  for (size_t p = start; p < text.size(); ++p) {
    if (text[p] == '\n') ++lines;
  }
  text += "def ";
  text += fn;
  text += "():" + eol + "    pass" + eol;
  at.line = lines + 1;
  at.column = 5;
  at.added = true;
  return at;
}

}  // namespace yc::core
