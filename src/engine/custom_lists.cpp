// SPDX-License-Identifier: Apache-2.0
// custom_lists.cpp - AutoFill / Sort lists
#include "engine/custom_lists.h"

#include <cctype>
#include <mutex>

namespace yc::engine {

namespace {
std::mutex g_mu;
std::vector<FillList> g_user;

std::string LowerAscii(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
}  // namespace

// Excel's default lists + Chinese lists stored as UTF-8 escapes
const std::vector<FillList>& BuiltinFillLists() {
  static const std::vector<FillList> lists = {
      {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
      {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
      {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
      {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
      {"\346\230\237\346\234\237\346\227\245", "\346\230\237\346\234\237\344\270\200", "\346\230\237\346\234\237\344\272\214", "\346\230\237\346\234\237\344\270\211", "\346\230\237\346\234\237\345\233\233", "\346\230\237\346\234\237\344\272\224", "\346\230\237\346\234\237\345\205\255"},
      {"\351\200\261\346\227\245", "\351\200\261\344\270\200", "\351\200\261\344\272\214", "\351\200\261\344\270\211", "\351\200\261\345\233\233", "\351\200\261\344\272\224", "\351\200\261\345\205\255"},
      {"\344\270\200\346\234\210", "\344\272\214\346\234\210", "\344\270\211\346\234\210", "\345\233\233\346\234\210", "\344\272\224\346\234\210", "\345\205\255\346\234\210", "\344\270\203\346\234\210", "\345\205\253\346\234\210", "\344\271\235\346\234\210", "\345\215\201\346\234\210", "\345\215\201\344\270\200\346\234\210", "\345\215\201\344\272\214\346\234\210"},
      {"\347\254\254\344\270\200\345\255\243", "\347\254\254\344\272\214\345\255\243", "\347\254\254\344\270\211\345\255\243", "\347\254\254\345\233\233\345\255\243"},
      {"\345\255\220", "\344\270\221", "\345\257\205", "\345\215\257", "\350\276\260", "\345\267\263", "\345\215\210", "\346\234\252", "\347\224\263", "\351\205\211", "\346\210\214", "\344\272\245"},
      {"\347\224\262", "\344\271\231", "\344\270\231", "\344\270\201", "\346\210\212", "\345\267\261", "\345\272\232", "\350\276\233", "\345\243\254", "\347\231\270"},
  };
  return lists;
}

std::vector<FillList> UserFillLists() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_user;
}

void SetUserFillLists(std::vector<FillList> lists) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_user = std::move(lists);
}

std::vector<FillList> AllFillLists() {
  std::vector<FillList> all = BuiltinFillLists();
  for (auto& l : UserFillLists()) {
    if (!l.empty()) all.push_back(std::move(l));
  }
  return all;
}

bool FindInFillLists(const std::string& s, int* list, int* idx) {
  const auto all = AllFillLists();
  const std::string low = LowerAscii(s);
  for (size_t i = 0; i < all.size(); ++i) {
    for (size_t j = 0; j < all[i].size(); ++j) {
      if (LowerAscii(all[i][j]) == low) {
        *list = static_cast<int>(i);
        *idx = static_cast<int>(j);
        return true;
      }
    }
  }
  return false;
}

}  // namespace yc::engine
