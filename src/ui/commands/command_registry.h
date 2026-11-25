// SPDX-License-Identifier: Apache-2.0
// command_registry.h - command system (PLAN §11.5: single source)
//
// The ribbon, context menus, shortcuts, the QAT and Python (ExecuteMso) all trigger the same command through its id.
// Command ids follow idMso style (Bold, PasteSpecial, MergeCenter...); schema/commands.yaml lists
// "command -> Python entry point" and scripts/check_commands.py checks the coverage (PLAN §13.5a).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace yc::ui {

struct Command {
  std::string id;
  std::string label;        // text on the ribbon (may contain '\n' for two lines)
  std::string tip_title;    // ScreenTip title (empty = use label)
  std::string tip_desc;     // ScreenTip description
  std::string shortcut;     // for display, e.g. "Ctrl+B"
  const char* icon = nullptr;
  bool toggle = false;      // toggle button (B, I, U...)
  bool pressed = false;     // toggle state (prototype: stored here; to be derived from UiContext, PLAN §11.6)
  bool enabled = true;
  std::function<void(Command&)> execute;
};

class CommandRegistry {
 public:
  Command& Add(Command c);
  Command* Find(const std::string& id);
  // run a command; returns whether it was found and run (a command the guard refuses counts as run: it was handled).
  bool Execute(const std::string& id);
  // asked before every command runs, wherever it comes from (ribbon, menus, keys, QAT, ExecuteMso): false refuses it
  // and the guard says why (read-only mode refuses the commands that change the workbook, before their dialogs open)
  std::function<bool(const Command&)> guard;

  const std::string& last_executed() const { return last_; }
  // called after every command that runs (and for NoteLast): the app keeps what Repeat (F4) repeats with it
  std::function<void(const std::string&)> on_run;
  // an action that isn't a command but counts as one, recorded as the last one (the Delete key's Clear Contents)
  void NoteLast(const std::string& id) {
    last_ = id;
    if (on_run) on_run(id);
  }
  int execute_count() const { return count_; }
  const std::vector<std::unique_ptr<Command>>& all() const { return list_; }

 private:
  std::vector<std::unique_ptr<Command>> list_;
  std::unordered_map<std::string, Command*> by_id_;
  std::string last_;
  int count_ = 0;
};

CommandRegistry& Commands();

}  // namespace yc::ui
