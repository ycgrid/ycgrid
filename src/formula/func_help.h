// SPDX-License-Identifier: Apache-2.0
// func_help.h - function help (shared by formula autocomplete, argument hints, the Insert Function dialog and API lookup)
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yc::formula {

struct FuncHelp {
  const char* name;      // uppercase
  const char* args;      // argument list (as in Excel), e.g. "number1, [number2], ..."
  const char* desc;      // one-sentence description
  const char* category;  // category
};

const FuncHelp* FindFuncHelp(std::string_view upper_name);
const std::vector<FuncHelp>& AllFuncHelp();
// category list (drop-down of the Insert Function dialog)
std::vector<const char*> FuncCategories();
// what an argument expects (Function Arguments shows it under the function's description while its box has the
// keyboard): fn uppercase, arg as the argument list names it (number1, [range_lookup]); "" when it isn't described
const char* FuncArgHelp(std::string_view fn, std::string_view arg);

// worksheet functions written in Python (@udf): their argument list, description, category ("User Defined" unless
// Application.MacroOptions gives one) and argument descriptions, found by FindFuncHelp, FuncArgHelp and
// FuncCategories as the built-in ones are (Insert Function, the argument hints). The list replaces the previous one;
// what was handed out before stays valid.
struct UserFunc {
  std::string name, args, desc, category;
  std::vector<std::pair<std::string, std::string>> arg_help;  // (argument's name, its description)
};
void SetUserFuncHelp(std::vector<UserFunc> funcs);
std::vector<FuncHelp> UserFuncHelp();  // the entries, for a list (pointers that stay valid)

}  // namespace yc::formula
