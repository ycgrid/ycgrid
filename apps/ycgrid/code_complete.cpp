// SPDX-License-Identifier: Apache-2.0
// code_complete.cpp - IntelliSense for one-line Python input (the Immediate window): completions from the Python host's
// namespace (variables, the Excel object model, members after a dot), asked for in the background while typing, shown
// in a list above the input; Up / Down choose, Tab or Enter inserts, Esc closes. Without a list, Up / Down walk the
// history of lines run.
#include "app_state.h"
#include "code_complete.h"
#include "core/i18n.h"
#include "python_bridge.h"

#include "fonts/fonts.h"
#include "frame/frame_context.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

namespace yc::app {

using io::MV;
using ui::S;

namespace {

bool IsWordChar(char c) { return c == '_' || (static_cast<unsigned char>(c) >= '0' && static_cast<unsigned char>(c) <= '9') ||
                                 (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

std::shared_ptr<std::future<MV>> Ask(PythonBridge* py, const std::string& code) {
  auto prom = std::make_shared<std::promise<MV>>();
  auto fut = std::make_shared<std::future<MV>>(prom->get_future());
  std::thread([prom, py, code] {
    MV p = MV::Map();
    p.Set("kind", MV::Str("complete"));
    p.Set("code", MV::Str(code));
    try {
      prom->set_value(py->RunHostJob(std::move(p), 3000));
    } catch (...) {
      prom->set_value(MV());
    }
  }).detach();
  return fut;
}

int Callback(ImGuiInputTextCallbackData* d) {
  auto* c = static_cast<CodeComplete*>(d->UserData);
  const bool list = c->open && !c->items.empty();
  if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
    const std::string now(d->Buf, static_cast<size_t>(d->BufTextLen));
    if (now != c->text || d->CursorPos != c->caret) {
      if (now != c->text) c->changed_at = ImGui::GetTime();
      c->text = now;
      c->caret = d->CursorPos;
      // a character that can't continue a name or start a member closes the list
      const char last = c->caret > 0 ? now[static_cast<size_t>(c->caret) - 1] : ' ';
      if (!IsWordChar(last) && last != '.') c->open = false;
    }
    if (c->insert) {  // Enter accepted an item (handled outside the callback): replace the word with it
      c->insert = false;
      const int from = std::clamp(c->start, 0, d->CursorPos);
      d->DeleteChars(from, d->CursorPos - from);
      d->InsertChars(from, c->insert_text.c_str());
      c->open = false;
    }
  } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion && list) {
    const int from = std::clamp(c->start, 0, d->CursorPos);
    d->DeleteChars(from, d->CursorPos - from);
    d->InsertChars(from, c->items[static_cast<size_t>(c->sel)].name.c_str());
    c->open = false;
  } else if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    const int n = static_cast<int>(c->items.size());
    if (list) {
      c->sel = d->EventKey == ImGuiKey_UpArrow ? (c->sel + n - 1) % n : (c->sel + 1) % n;
      c->scroll = true;
    } else if (!c->history.empty()) {
      const int h = static_cast<int>(c->history.size());
      if (d->EventKey == ImGuiKey_UpArrow) c->hist = c->hist < 0 ? h - 1 : std::max(0, c->hist - 1);
      else c->hist = c->hist < 0 ? -1 : (c->hist + 1 >= h ? -1 : c->hist + 1);
      d->DeleteChars(0, d->BufTextLen);
      if (c->hist >= 0) d->InsertChars(0, c->history[static_cast<size_t>(c->hist)].c_str());
    }
  }
  return 0;
}

}  // namespace

bool CodeInput(AppState& s, const char* id, const char* hint, char* buf, size_t size, CodeComplete& c) {
  const bool list = c.open && !c.items.empty();
  if (list && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    c.open = false;
    // the list takes this Esc: the input keeps the focus (and its text)
    ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##yc_complete_esc"), ImGuiInputFlags_LockThisFrame);
  }
  // Enter with the list open inserts the item instead of running the line
  const bool accept = list && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
  if (accept) {
    c.insert = true;
    c.insert_text = c.items[static_cast<size_t>(c.sel)].name;
  }
  if (c.refocus) {  // an item was clicked: back into the input, where the callback inserts it
    ImGui::SetKeyboardFocusHere();
    c.refocus = false;
  }
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways |
                                    ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
  bool run = ImGui::InputTextWithHint(id, hint, buf, size, flags, Callback, &c);
  const ImRect field(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  const bool active = ImGui::IsItemActive();
  if (accept) {
    run = false;
    ImGui::SetKeyboardFocusHere(-1);
  }
  if (run) {
    if (c.history.empty() || c.history.back() != buf) c.history.emplace_back(buf);
    if (c.history.size() > 200) c.history.erase(c.history.begin());
    c.hist = -1;
    c.open = false;
    c.items.clear();
  }
  // ask the host after a short pause in typing (never blocks the frame)
  const double now = ImGui::GetTime();
  if (c.job && c.job->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const MV r = c.job->get();
    c.job.reset();
    const MV* res = r.GetBool("ok", false) ? r.Get("result") : nullptr;
    if (res && c.asked == c.text.substr(0, static_cast<size_t>(std::clamp(c.caret, 0, static_cast<int>(c.text.size()))))) {
      c.items.clear();
      c.start = static_cast<int>(res->GetInt("start", c.caret));
      if (const MV* it = res->Get("items")) {
        for (const auto& x : it->a) c.items.push_back({x.GetStr("name"), x.GetStr("kind"), x.GetStr("doc")});
      }
      c.sel = 0;
      // a single item that is already typed in full: nothing to offer
      const std::string typed = c.text.substr(static_cast<size_t>(std::clamp(c.start, 0, c.caret)), static_cast<size_t>(c.caret - std::clamp(c.start, 0, c.caret)));
      c.open = !c.items.empty() && !(c.items.size() == 1 && c.items[0].name == typed);
    }
  }
  if (active && !c.job && s.python && s.python->HostReady() && now - c.changed_at > 0.12 && c.caret > 0) {
    const std::string head = c.text.substr(0, static_cast<size_t>(std::min<int>(c.caret, static_cast<int>(c.text.size()))));
    const char last = head.empty() ? ' ' : head.back();
    if (head != c.asked && (IsWordChar(last) || last == '.')) {
      c.asked = head;
      c.job = Ask(s.python.get(), head);
    }
  }
  if (!active && !accept && !c.refocus && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) c.open = false;
  // the list, above the input (the Immediate window keeps its input at the bottom)
  if (c.open && !c.items.empty()) {
    const int shown = std::min<int>(10, static_cast<int>(c.items.size()));
    const float row = ImGui::GetTextLineHeightWithSpacing();
    const float x = field.Min.x + ImGui::GetStyle().FramePadding.x +
                    ImGui::CalcTextSize(c.text.c_str(), c.text.c_str() + std::clamp(c.start, 0, static_cast<int>(c.text.size()))).x;
    ImGui::SetNextWindowPos(ImVec2(std::min(x, field.Max.x - S(200)), field.Min.y - 2), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##yc_code_complete", nullptr, wf)) {
      ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
      c.sel = std::clamp(c.sel, 0, static_cast<int>(c.items.size()) - 1);
      ImGui::BeginChild("##items", ImVec2(S(300), row * static_cast<float>(shown) + 4), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
      for (size_t k = 0; k < c.items.size(); ++k) {
        const auto& it = c.items[k];
        ImGui::PushID(static_cast<int>(k));
        const char* tag = it.kind == "function" ? "f" : it.kind == "class" ? "C" : it.kind == "module" ? "M" : it.kind == "property" ? "P"
                          : it.kind == "keyword" ? "k" : "v";
        ImGui::TextDisabled("%s", tag);
        ImGui::SameLine(S(22));
        if (ImGui::Selectable(it.name.c_str(), static_cast<int>(k) == c.sel)) {
          c.insert = true;
          c.insert_text = it.name;
          c.refocus = true;
        }
        if (static_cast<int>(k) == c.sel && c.scroll) ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
      }
      c.scroll = false;
      ImGui::EndChild();
      const auto& cur = c.items[static_cast<size_t>(c.sel)];
      if (!cur.doc.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(300));
        ImGui::TextUnformatted(cur.doc.c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::TextDisabled("%s", TR("Tab or Enter inserts, ↑↓ selects, Esc closes"));
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }
  return run;
}

}  // namespace yc::app
