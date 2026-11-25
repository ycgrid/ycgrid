// SPDX-License-Identifier: Apache-2.0
// custom_lists.h - the lists AutoFill continues and Sort can order by (File > Options > Edit Custom Lists)
#pragma once

#include <string>
#include <vector>

namespace yc::engine {

using FillList = std::vector<std::string>;

// Excel's built-in lists (days, months, Chinese days, months, quarters, earthly branches, heavenly stems)
const std::vector<FillList>& BuiltinFillLists();
// the user's lists (kept in settings.json by the app); any thread
std::vector<FillList> UserFillLists();
void SetUserFillLists(std::vector<FillList> lists);
// built-in lists first, then the user's (Excel's custom list numbers are index + 1)
std::vector<FillList> AllFillLists();
// the list and position of a value (case-insensitive); false when no list has it
bool FindInFillLists(const std::string& s, int* list, int* idx);

}  // namespace yc::engine
