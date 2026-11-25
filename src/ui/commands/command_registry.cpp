// SPDX-License-Identifier: Apache-2.0
#include "commands/command_registry.h"

namespace yc::ui {

Command& CommandRegistry::Add(Command c) {
  auto it = by_id_.find(c.id);
  if (it != by_id_.end()) {
    *it->second = std::move(c);  // registered twice: the last one wins
    return *it->second;
  }
  list_.push_back(std::make_unique<Command>(std::move(c)));
  Command* p = list_.back().get();
  by_id_.emplace(p->id, p);
  return *p;
}

Command* CommandRegistry::Find(const std::string& id) {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

bool CommandRegistry::Execute(const std::string& id) {
  Command* c = Find(id);
  if (!c || !c->enabled) return false;
  if (guard && !guard(*c)) return true;  // refused (a toggle keeps its state; Repeat doesn't take it up)
  if (c->toggle) c->pressed = !c->pressed;
  if (c->execute) c->execute(*c);
  last_ = id;
  ++count_;
  if (on_run) on_run(id);
  return true;
}

CommandRegistry& Commands() {
  static CommandRegistry registry;
  return registry;
}

}  // namespace yc::ui
