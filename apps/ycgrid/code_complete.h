// SPDX-License-Identifier: Apache-2.0
// code_complete.h - IntelliSense for one-line Python input (see code_complete.cpp)
#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "io/msgvalue.h"

namespace yc::app {

struct AppState;

struct CodeComplete {
  struct Item {
    std::string name, kind, doc;
  };
  std::string text, asked, insert_text;
  int caret = 0, start = 0, sel = 0, hist = -1;
  bool open = false, insert = false, scroll = false, refocus = false;
  double changed_at = 0;
  std::vector<Item> items;
  std::vector<std::string> history;
  std::shared_ptr<std::future<io::MV>> job;
};

// an input line with completions from the Python host; returns true when Enter runs the line (not when it inserts)
bool CodeInput(AppState& s, const char* id, const char* hint, char* buf, size_t size, CodeComplete& c);

}  // namespace yc::app
