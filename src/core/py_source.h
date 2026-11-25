// SPDX-License-Identifier: Apache-2.0
// py_source.h - small edits YCGrid makes to a workbook's Python modules
#pragma once

#include <string>
#include <string_view>

namespace yc::core {

// True when `name` can be a Python function name: ASCII letters, digits and `_`, and any character beyond ASCII
// (Python takes letters of every script; YCGrid leaves the finer check to Python), not starting with a digit.
bool IsPythonName(std::string_view name);

// Where the editor's caret goes for a handler: a 1-based line and column.
struct HandlerPlace {
  int line = 1;
  int column = 1;
  bool added = false;  // the handler wasn't there and was appended to the text
};

// Excel's View Code on a control: finds `def <fn>(` at the start of a line of a Python module's text (UTF-8), or,
// when there is none, appends an empty one (`def <fn>():` and `pass`, two blank lines before it, the text's own line
// ends). The place is the handler's first body line: the `pass` of a new one, the line after `def` of an old one.
// `text` changes only when the handler is added; `fn` must be a Python name (IsPythonName).
HandlerPlace EnsurePythonHandler(std::string& text, std::string_view fn);

}  // namespace yc::core
