// SPDX-License-Identifier: Apache-2.0
// ui_custom.cpp - the Quick Access Toolbar, ribbon display options, command search (Alt+Q), context menus customized
// from Python (CommandBars("Cell").Controls.Add) and Application.Dialogs
#include "app_state.h"
#include "core/i18n.h"
#include "python_bridge.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "ribbon/ribbon.h"
#include "titlebar/titlebar.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace yc::app {

using ui::S;

namespace {

std::string Label(const std::string& id) {
  const ui::Command* c = ui::Commands().Find(id);
  std::string l = c ? c->label : id;
  std::replace(l.begin(), l.end(), '\n', ' ');
  if (auto k = l.find("##"); k != std::string::npos) l.erase(k);
  return l;
}

void SaveUi(AppState& s) {
  if (!SaveSettings()) s.SetStatus(TR("Couldn't save the settings: ") + ToUtf8(SettingsPath()), 8.0);
}

bool InQat(const std::string& id) {
  const auto& q = Config().qat;
  return std::find(q.begin(), q.end(), id) != q.end();
}

void ToggleQat(AppState& s, const std::string& id) {
  auto& q = Config().qat;
  auto it = std::find(q.begin(), q.end(), id);
  if (it != q.end()) q.erase(it);
  else q.push_back(id);
  SaveUi(s);
}

void SetRibbonMode(AppState& s, int mode) {
  Config().ribbon_mode = mode;
  auto& st = ui::ribbon::St();
  st.collapsed = mode == 1;
  st.peek = false;
  s.ribbon_revealed = false;
  SaveUi(s);
}

// the common commands of Excel's Customize Quick Access Toolbar drop-down
const char* const kQatCommon[] = {"FileNew", "FileOpen", "Save", "QuickPrint", "PrintPreview", "Spelling", "Undo", "Redo", "SortAscending",
                                  "SortDescending", "ToggleFilter", "Calculate"};

// ---- Customize Quick Access Toolbar (More Commands...) ----
struct QatDlg {
  char filter[128] = {};
  int sel_all = -1, sel_qat = -1;
};
QatDlg g_qat;

// ---- command search (Alt+Q) ----
struct Search {
  bool open = false, focus = false;
  char text[128] = {};
  int sel = 0;
};
Search g_search;

bool Contains(const std::string& hay, const std::string& needle) {
  std::string h = hay, n = needle;
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return h.find(n) != std::string::npos;
}

std::vector<std::string> SearchCommands(const std::string& q) {
  std::vector<std::pair<int, std::string>> hits;
  if (q.empty()) return {};
  for (const auto& c : ui::Commands().all()) {
    if (!c->execute || c->label.empty()) continue;
    const std::string l = Label(c->id);
    int score = -1;
    if (Contains(l, q)) score = Contains(l.substr(0, q.size()), q) ? 0 : 1;
    else if (Contains(c->id, q)) score = 2;
    else if (Contains(c->tip_desc, q)) score = 3;
    if (score >= 0) hits.emplace_back(score, c->id);
  }
  std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> out;
  for (const auto& [sc, id] : hits) {
    if (out.size() >= 12) break;
    // a submenu-only command (a drop-down) does nothing on its own
    if (id.size() > 4 && id.compare(id.size() - 4, 4, "Menu") == 0) continue;
    out.push_back(id);
  }
  return out;
}

// ---- context menus customized from Python ----
struct MenuControl {
  int id = 0;
  int type = 1;  // msoControlButton 1, msoControlPopup 10
  std::string caption, on_action, tag, parameter;
  bool begin_group = false, enabled = true, visible = true;
  std::vector<MenuControl> controls;
};
std::map<std::string, std::vector<MenuControl>> g_bars;  // "Cell", "Row", "Column", "Ply"
int g_next_control = 1;
std::string g_action_bar;
int g_action_id = 0;

MenuControl* FindControl(std::vector<MenuControl>& v, int id) {
  for (auto& c : v) {
    if (c.id == id) return &c;
    if (MenuControl* x = FindControl(c.controls, id)) return x;
  }
  return nullptr;
}

bool EraseControl(std::vector<MenuControl>& v, int id) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].id == id) {
      v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
    if (EraseControl(v[i].controls, id)) return true;
  }
  return false;
}

io::MV ControlToMV(const MenuControl& c) {
  io::MV x = io::MV::Map();
  x.Set("id", io::MV::Int(c.id));
  x.Set("type", io::MV::Int(c.type));
  x.Set("caption", io::MV::Str(c.caption));
  x.Set("on_action", io::MV::Str(c.on_action));
  x.Set("tag", io::MV::Str(c.tag));
  x.Set("parameter", io::MV::Str(c.parameter));
  x.Set("begin_group", io::MV::Bool(c.begin_group));
  x.Set("enabled", io::MV::Bool(c.enabled));
  x.Set("visible", io::MV::Bool(c.visible));
  io::MV kids = io::MV::Array();
  for (const auto& k : c.controls) kids.Push(ControlToMV(k));
  x.Set("controls", std::move(kids));
  return x;
}

void DrawControls(AppState& s, const std::string& bar, const std::vector<MenuControl>& v, bool top = false) {
  bool first = true;
  for (const auto& c : v) {
    if (!c.visible) continue;
    if (c.begin_group && !(top && first)) ImGui::Separator();  // the menu already has a separator before the added items
    first = false;
    ImGui::PushID(c.id);
    if (c.type == 10) {
      if (ImGui::BeginMenu(c.caption.c_str(), c.enabled)) {
        DrawControls(s, bar, c.controls);
        ImGui::EndMenu();
      }
    } else if (ImGui::MenuItem(c.caption.c_str(), nullptr, false, c.enabled)) {
      if (!c.on_action.empty() && s.python) {
        g_action_bar = bar;
        g_action_id = c.id;
        s.python->RunMacro(c.on_action);
      }
    }
    ImGui::PopID();
  }
}

// ---- Application.Dialogs(xlDialog...).Show: Excel's dialog numbers -> commands ----
const std::map<int, const char*>& DialogCommands() {
  static const std::map<int, const char*> m = {
      {1, "FileOpen"},           {5, "FileSaveAs"},          {7, "PageSetupDialog"},        {8, "FilePrint"},
      {28, "ProtectSheet"},      {39, "CustomSort"},         {42, "FormatCellsNumberDialog"}, {43, "FormatCellsAlignmentDialog"},
      {44, "NewCellStyle"},      {45, "FormatCellsDialog"},  {46, "FormatCellsDialog"},     {47, "ColumnWidth"},
      {53, "PasteSpecial"},      {54, "DeleteCells"},        {55, "InsertCells"},           {58, "PasteName"},
      {61, "DefineName"},        {62, "CreateNamesFromSelection"}, {63, "GoTo"},          {64, "Find"},
      {84, "FormatCellsDialog"}, {119, "FileNew"},           {127, "RowHeight"},            {130, "Replace"},
      {150, "FormatCellsFontDialog"}, {191, "Consolidate"},  {198, "GoalSeek"},             {222, "PrintPreview"},
      {256, "ZoomDialog"},       {381, "FormatCellsFontDialog"}, {450, "InsertFunction"},  {525, "DataValidation"},
      {583, "CfManage"},         {596, "InsertHyperlink"},   {1026, "NameManager"},
  };
  return m;
}

}  // namespace

std::vector<std::string> QatCommands() { return Config().qat; }

namespace {
struct Pane {
  std::string command;
  std::function<bool&()> flag;
};
std::map<std::string, Pane>& Panes() {
  static std::map<std::string, Pane> m;
  return m;
}
}  // namespace

void RegisterPane(const std::string& name, const std::string& command, std::function<bool&()> flag) {
  Panes()[name] = Pane{command, std::move(flag)};
}

std::map<std::string, bool> PaneStates() {
  std::map<std::string, bool> out;
  for (auto& [n, p] : Panes()) out[n] = p.flag();
  return out;
}

void SetPaneState(const std::string& name, bool shown) {
  auto it = Panes().find(name);
  if (it != Panes().end()) it->second.flag() = shown;
}

void RegisterUiCustomCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("CustomizeQat", TR("Customize Quick Access Toolbar..."), TR("Choose the commands on the Quick Access Toolbar."))).execute =
      [&s](ui::Command&) { s.OpenDialog(Dialog::CustomizeQat); };
  R.Add(make("RibbonAlwaysShow", TR("Always show Ribbon"))).execute = [&s](ui::Command&) { SetRibbonMode(s, 0); };
  R.Add(make("RibbonTabsOnly", TR("Show tabs only"))).execute = [&s](ui::Command&) { SetRibbonMode(s, 1); };
  R.Add(make("RibbonAutoHide", TR("Auto-hide Ribbon"))).execute = [&s](ui::Command&) { SetRibbonMode(s, 2); };
  R.Add(make("CommandSearch", TR("Search"), TR("Find a command by name and run it."))).execute = [](ui::Command&) {
    g_search.open = g_search.focus = true;
    g_search.text[0] = 0;
    g_search.sel = 0;
  };
  ui::ribbon::St().collapsed = Config().ribbon_mode == 1;

  // right-click on a ribbon command
  ui::ribbon::St().item_menu = [&s](const std::string& cmd) {
    if (InQat(cmd)) {
      if (ImGui::MenuItem(TR("Remove from Quick Access Toolbar"))) ToggleQat(s, cmd);
    } else if (ImGui::MenuItem(TR("Add to Quick Access Toolbar"))) {
      ToggleQat(s, cmd);
    }
    if (ImGui::MenuItem(TR("Customize Quick Access Toolbar..."))) s.OpenDialog(Dialog::CustomizeQat);
    ImGui::Separator();
    const bool collapsed = ui::ribbon::St().collapsed;
    if (ImGui::MenuItem(collapsed ? TR("Pin the Ribbon") : TR("Collapse the Ribbon"))) SetRibbonMode(s, collapsed ? 0 : 1);
  };
  // the drop-down after the Quick Access Toolbar
  ui::titlebar::St().qat_menu = [&s] {
    ImGui::TextDisabled("%s", TR("Customize Quick Access Toolbar"));
    for (const char* id : kQatCommon) {
      if (!ui::Commands().Find(id)) continue;
      if (ImGui::MenuItem(Label(id).c_str(), nullptr, InQat(id))) ToggleQat(s, id);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(TR("More Commands..."))) s.OpenDialog(Dialog::CustomizeQat);
    ImGui::Separator();
    const int mode = Config().ribbon_mode;
    if (ImGui::MenuItem(TR("Always show Ribbon"), nullptr, mode == 0)) SetRibbonMode(s, 0);
    if (ImGui::MenuItem(TR("Show tabs only"), nullptr, mode == 1)) SetRibbonMode(s, 1);
    if (ImGui::MenuItem(TR("Auto-hide Ribbon"), nullptr, mode == 2)) SetRibbonMode(s, 2);
  };
}

void UpdateUiCustom(AppState& s) {
  // Ctrl+F1 / double-clicking a tab switch between "always" and "tabs only"
  auto& st = ui::ribbon::St();
  auto& cfg = Config();
  if (cfg.ribbon_mode != 2 && (cfg.ribbon_mode == 1) != st.collapsed) {
    cfg.ribbon_mode = st.collapsed ? 1 : 0;
    SaveUi(s);
  }
  if (ui::titlebar::St().search_open_request) {
    ui::titlebar::St().search_open_request = false;
    ui::Commands().Execute("CommandSearch");
  }
}

bool RibbonHidden(AppState& s) { return Config().ribbon_mode == 2 && !s.ribbon_revealed && !ui::ribbon::St().backstage; }

void DrawUiCustom(AppState& s) {
  // ---- command search ----
  if (g_search.open) {
    const ImRect box = ui::titlebar::St().search_box;
    const ImVec2 pos = box.GetWidth() > 0 ? box.Min : ImVec2(ImGui::GetMainViewport()->GetCenter().x - S(200), ImGui::GetMainViewport()->Pos.y + S(4));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(std::max(box.GetWidth(), S(400)), 0));
    if (!ImGui::IsPopupOpen("##cmdsearch")) ImGui::OpenPopup("##cmdsearch");
    if (ImGui::BeginPopup("##cmdsearch")) {
      // Escape closes the search even while its box has the keyboard (the box owns the key), and a dialog opened
      // meanwhile (by a shortcut, a macro, Python) takes over: a search left open under it kept Escape from the dialog
      const ImGuiKeyData* esc = ImGui::GetKeyData(ImGuiKey_Escape);
      if ((esc->Down && esc->DownDuration == 0.0f) || DialogOpen(s)) {
        g_search.open = false;
        ImGui::ClearActiveID();
        ImGui::CloseCurrentPopup();
      } else {
        if (g_search.focus) {
          ImGui::SetKeyboardFocusHere();
          g_search.focus = false;
        }
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputTextWithHint("##q", TR("Search for commands"), g_search.text, sizeof g_search.text, ImGuiInputTextFlags_EnterReturnsTrue);
        const auto hits = SearchCommands(g_search.text);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) g_search.sel = std::min(g_search.sel + 1, static_cast<int>(hits.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) g_search.sel = std::max(g_search.sel - 1, 0);
        g_search.sel = std::clamp(g_search.sel, 0, std::max(0, static_cast<int>(hits.size()) - 1));
        std::string run;
        if (g_search.text[0] && hits.empty()) ImGui::TextDisabled("%s", TR("No matching commands."));
        for (size_t i = 0; i < hits.size(); ++i) {
          const ui::Command* c = ui::Commands().Find(hits[i]);
          std::string l = Label(hits[i]);
          if (c && !c->shortcut.empty()) l += "    " + c->shortcut;
          ImGui::BeginDisabled(c && !c->enabled);
          if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == g_search.sel)) run = hits[i];
          ImGui::EndDisabled();
        }
        if (enter && !hits.empty()) run = hits[static_cast<size_t>(g_search.sel)];
        if (!run.empty()) {
          g_search.open = false;
          ImGui::CloseCurrentPopup();
          ui::Commands().Execute(run);
        }
      }
      ImGui::EndPopup();
    } else {
      g_search.open = false;
    }
  }
  // ---- Customize Quick Access Toolbar ----
  if (s.dialog != Dialog::CustomizeQat) return;
  const char* title = TR("Quick Access Toolbar");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(700), S(480)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) return;
  auto& q = Config().qat;
  QatDlg& d = g_qat;
  // every command with a label, sorted by name
  std::vector<std::pair<std::string, std::string>> all;
  for (const auto& c : ui::Commands().all()) {
    if (c->label.empty() || !c->execute) continue;
    const std::string l = Label(c->id);
    if (d.filter[0] && !Contains(l, d.filter) && !Contains(c->id, d.filter)) continue;
    all.emplace_back(l, c->id);
  }
  std::sort(all.begin(), all.end());
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Choose commands from:"));
  ImGui::SetNextItemWidth(S(280));
  ImGui::InputTextWithHint("##filter", TR("Filter"), d.filter, sizeof d.filter);
  ImGui::BeginChild("##all", ImVec2(S(280), S(340)), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < all.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(all[i].first.c_str(), d.sel_all == static_cast<int>(i), ImGuiSelectableFlags_AllowDoubleClick)) {
      d.sel_all = static_cast<int>(i);
      if (ImGui::IsMouseDoubleClicked(0) && !InQat(all[i].second)) {
        q.push_back(all[i].second);
        SaveUi(s);
      }
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(1, S(120)));
  if (ImGui::Button(TR("Add >>"), ImVec2(S(100), 0)) && d.sel_all >= 0 && d.sel_all < static_cast<int>(all.size()) && !InQat(all[static_cast<size_t>(d.sel_all)].second)) {
    q.push_back(all[static_cast<size_t>(d.sel_all)].second);
    SaveUi(s);
  }
  if (ImGui::Button(TR("<< Remove"), ImVec2(S(100), 0)) && d.sel_qat >= 0 && d.sel_qat < static_cast<int>(q.size())) {
    q.erase(q.begin() + d.sel_qat);
    d.sel_qat = -1;
    SaveUi(s);
  }
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Quick Access Toolbar:"));
  ImGui::BeginChild("##qat", ImVec2(S(220), S(370)), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < q.size(); ++i) {
    ImGui::PushID(static_cast<int>(i) + 10000);
    if (ImGui::Selectable(Label(q[i]).c_str(), d.sel_qat == static_cast<int>(i))) d.sel_qat = static_cast<int>(i);
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(1, S(140)));
  if (ImGui::ArrowButton("##up", ImGuiDir_Up) && d.sel_qat > 0) {
    std::swap(q[static_cast<size_t>(d.sel_qat)], q[static_cast<size_t>(d.sel_qat) - 1]);
    --d.sel_qat;
    SaveUi(s);
  }
  if (ImGui::ArrowButton("##down", ImGuiDir_Down) && d.sel_qat >= 0 && d.sel_qat + 1 < static_cast<int>(q.size())) {
    std::swap(q[static_cast<size_t>(d.sel_qat)], q[static_cast<size_t>(d.sel_qat) + 1]);
    ++d.sel_qat;
    SaveUi(s);
  }
  ImGui::EndGroup();
  ImGui::Separator();
  if (ImGui::Button(TR("Reset"))) {
    q = {"Save", "Undo", "Redo", "Screenshot"};
    SaveUi(s);
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - S(112));
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (DlgOkButton(TR("OK"), ImVec2(S(96), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// ---- custom context menu items (from Python) ----

void DrawCustomMenuItems(AppState& s, const char* bar) {
  auto it = g_bars.find(bar);
  if (it == g_bars.end() || it->second.empty()) return;
  ImGui::Separator();
  DrawControls(s, bar, it->second, true);
}

io::MV CommandBarRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err) {
  // ---- yc.ui: panes, ribbon display, the Quick Access Toolbar, the command list ----
  if (m == "cmdbar.pane") {
    const std::string name = p.GetStr("name");
    if (name.empty()) {  // the names and whether each pane shows
      io::MV x = io::MV::Map();
      for (auto& [n, pane] : Panes()) x.Set(n.c_str(), io::MV::Bool(pane.flag()));
      return x;
    }
    auto it = Panes().find(name);
    if (it == Panes().end()) {
      std::string names;
      for (const auto& [n, pane] : Panes()) names += (names.empty() ? "" : ", ") + n;
      *err = "Unknown pane: " + name + " (" + names + ")";
      return {};
    }
    Pane& pane = it->second;
    if (p.Has("visible")) {
      const bool want = p.GetBool("visible");
      if (pane.flag() != want) {
        if (pane.command.empty()) pane.flag() = want;
        else ui::Commands().Execute(pane.command);
      }
      ui::RequestRedrawIn(0.0);
    }
    return io::MV::Bool(pane.flag());
  }
  if (m == "cmdbar.ribbon_mode") {  // 0 always show, 1 tabs only, 2 auto-hide
    if (p.Has("mode")) {
      const int mode = static_cast<int>(p.GetInt("mode"));
      if (mode < 0 || mode > 2) {
        *err = "The ribbon display is 0 (always show), 1 (tabs only) or 2 (auto-hide)";
        return {};
      }
      SetRibbonMode(s, mode);
    }
    return io::MV::Int(Config().ribbon_mode);
  }
  if (m == "cmdbar.qat") {
    if (const io::MV* ids = p.Get("ids")) {
      std::vector<std::string> q;
      for (const io::MV& v : ids->a) {
        if (!ui::Commands().Find(v.s)) {
          *err = "Unknown command: " + v.s;
          return {};
        }
        q.push_back(v.s);
      }
      Config().qat = q;
      SaveUi(s);
    }
    io::MV a = io::MV::Array();
    for (const auto& id : Config().qat) a.Push(io::MV::Str(id));
    return a;
  }
  if (m == "cmdbar.commands") {
    io::MV a = io::MV::Array();
    for (const auto& c : ui::Commands().all()) {
      io::MV x = io::MV::Map();
      x.Set("id", io::MV::Str(c->id));
      x.Set("label", io::MV::Str(Label(c->id)));
      x.Set("shortcut", io::MV::Str(c->shortcut));
      x.Set("toggle", io::MV::Bool(c->toggle));
      x.Set("pressed", io::MV::Bool(c->pressed));
      x.Set("enabled", io::MV::Bool(c->enabled));
      const int rb = ui::ribbon::ButtonIcon(c->id);
      x.Set("ribbon", io::MV::Bool(rb >= 0));
      x.Set("tab", io::MV::Str(ui::ribbon::TabOf(c->id)));  // the built-in tab it is on (in the UI's language)
      x.Set("icon", io::MV::Bool(rb >= 0 ? rb == 1 : c->icon != nullptr));
      x.Set("glyph", io::MV::Str(c->icon ? c->icon : ""));  // its icon (tests: no two commands of a tab share one)
      a.Push(std::move(x));
    }
    return a;
  }
  const std::string bar = p.GetStr("bar");
  static const char* const kBars[] = {"Cell", "Row", "Column", "Ply"};
  if (m != "cmdbar.action" && std::none_of(std::begin(kBars), std::end(kBars), [&](const char* b) { return bar == b; })) {
    *err = "Unknown command bar: " + bar + " (Cell, Row, Column and Ply can be customized)";
    return {};
  }
  auto& v = g_bars[bar];
  if (m == "cmdbar.get") {
    io::MV a = io::MV::Array();
    for (const auto& c : v) a.Push(ControlToMV(c));
    return a;
  }
  if (m == "cmdbar.add") {
    MenuControl c;
    c.id = g_next_control++;
    c.type = static_cast<int>(p.GetInt("type", 1));
    c.caption = p.GetStr("caption", c.type == 10 ? "Popup" : "Custom");
    c.parameter = p.GetStr("parameter");
    std::vector<MenuControl>* target = &v;
    if (const int parent = static_cast<int>(p.GetInt("parent")); parent) {
      MenuControl* pc = FindControl(v, parent);
      if (!pc || pc->type != 10) {
        *err = "The parent control isn't a popup";
        return {};
      }
      target = &pc->controls;
    }
    const int before = static_cast<int>(p.GetInt("before"));  // 1-based position, 0 = at the end
    const size_t pos = before > 0 ? std::min<size_t>(static_cast<size_t>(before) - 1, target->size()) : target->size();
    target->insert(target->begin() + static_cast<std::ptrdiff_t>(pos), c);
    return io::MV::Int(c.id);
  }
  if (m == "cmdbar.set") {
    MenuControl* c = FindControl(v, static_cast<int>(p.GetInt("id")));
    if (!c) {
      *err = "The control was deleted";
      return {};
    }
    if (p.Has("caption")) c->caption = p.GetStr("caption");
    if (p.Has("on_action")) c->on_action = p.GetStr("on_action");
    if (p.Has("tag")) c->tag = p.GetStr("tag");
    if (p.Has("parameter")) c->parameter = p.GetStr("parameter");
    if (p.Has("begin_group")) c->begin_group = p.GetBool("begin_group");
    if (p.Has("enabled")) c->enabled = p.GetBool("enabled");
    if (p.Has("visible")) c->visible = p.GetBool("visible");
    return {};
  }
  if (m == "cmdbar.delete") {
    EraseControl(v, static_cast<int>(p.GetInt("id")));
    return {};
  }
  if (m == "cmdbar.reset") {
    v.clear();
    return {};
  }
  if (m == "cmdbar.action") {
    io::MV x = io::MV::Map();
    x.Set("bar", io::MV::Str(g_action_bar));
    x.Set("id", io::MV::Int(g_action_id));
    return x;
  }
  *err = "Unknown request: " + m;
  return {};
}

// ---- Application.Dialogs ----
bool ShowBuiltinDialog(AppState& s, int number, std::string* err) {
  auto it = DialogCommands().find(number);
  if (it == DialogCommands().end() || !ui::Commands().Find(it->second)) {
    *err = "This dialog isn't available: " + std::to_string(number);
    return false;
  }
  // the Format Cells tabs
  if (number == 84) s.format_tab = 4;
  if (number == 45) s.format_tab = 3;
  if (number == 46) s.format_tab = 5;
  ui::Commands().Execute(it->second);
  return true;
}

bool DialogOpen(const AppState& s) { return s.dialog != Dialog::None || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId); }

}  // namespace yc::app
