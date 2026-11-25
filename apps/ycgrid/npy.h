// SPDX-License-Identifier: Apache-2.0
// npy.h - reading NumPy .npy column files (yc.data.save_columns): numbers, dates, booleans and fixed-width text,
// straight from disk into C++ (plots and the Data View don't pass through Python or the data channel)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yc::app {

struct NpyColumn {
  bool text = false, date = false;  // date: datetime64 converted to Unix seconds
  std::vector<double> num;          // numbers (NaN = missing)
  std::vector<std::string> str;     // text ('<U' columns)
};

// a 1-D array (or a column of a 2-D C-order array: column = its index); false with the reason on failure. A column
// preloaded by an NpyPreload of this path is taken from memory instead of the disk.
bool ReadNpy(const std::wstring& path, NpyColumn* out, std::string* error, int column = -1);

// Reads files on the calling thread (a request's connection thread) before the request goes to the UI thread, so
// opening twenty million rows doesn't stop the screen; ReadNpy there takes them. What isn't taken is dropped when
// the scope ends.
class NpyPreload {
 public:
  NpyPreload() = default;
  NpyPreload(const NpyPreload&) = delete;
  NpyPreload& operator=(const NpyPreload&) = delete;
  ~NpyPreload();
  void Add(const std::wstring& path, int column = -1);

 private:
  std::vector<std::pair<std::wstring, int>> keys_;
};

}  // namespace yc::app
