// SPDX-License-Identifier: Apache-2.0
// custom_ribbon.cpp - RibbonX: customUI.xml in the project folder adds tabs, groups and controls to the ribbon (PLAN §11.9)
//
// Supported: tab (id / idMso of a built-in tab), group, button, toggleButton, checkBox, splitButton, menu, menuSeparator, dropDown,
// comboBox, editBox, gallery, labelControl, separator, box / buttonGroup (flattened), dialogBoxLauncher; attributes label, size,
// imageMso, screentip, supertip, enabled, visible, tag, onAction, onChange, getLabel, getEnabled, getVisible, getPressed, getText,
// getSelectedItemIndex, getItemCount, getItemLabel, and customUI onLoad.
//
// Callbacks are Python functions ("Ribbon.OnStart" or "OnStart"). get* results are cached until IRibbonUI.Invalidate /
// InvalidateControl: the UI asks the host asynchronously and shows the last value (or the static attribute) meanwhile.
#include "app_state.h"
#include "core/i18n.h"
#include "python_bridge.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <memory>

namespace yc::app {

using io::MV;
using ui::S;

namespace {

struct RxItem {
  std::string id, label;
};

struct RxControl {
  std::string type;  // button, toggleButton, checkBox, splitButton, menu, dropDown, comboBox, editBox, gallery, labelControl, separator
  std::string id, id_mso, tag;
  std::string label, image_mso, image, screentip, supertip;
  std::string get_image;
  bool large = false;
  bool enabled = true, visible = true;
  std::string on_action, on_change;
  std::string get_label, get_enabled, get_visible, get_pressed, get_text, get_selected_index, get_item_count, get_item_label;
  std::vector<RxItem> items;          // dropDown, comboBox, gallery
  std::vector<RxControl> children;    // splitButton (button + menu), menu
  int width_chars = 0;                // sizeString
};

struct RxGroup {
  std::string id, label, get_visible;
  bool visible = true;
  std::vector<RxControl> controls;
  std::string launcher_action;
};

struct RxTab {
  std::string id, id_mso, label, get_visible;
  bool visible = true;
  std::vector<RxGroup> groups;
};

// run-time state of one control (stable addresses: check boxes point at pressed)
struct RxState {
  const RxControl* def = nullptr;
  bool pressed = false;
  std::string text;      // comboBox / editBox text, dropDown selection label
  int index = -1;        // dropDown / gallery selection
  std::vector<RxItem> items;
  char buf[256] = {};    // editBox / comboBox being edited
  bool editing = false;
};

struct Rx {
  // pictures for image / getImage: by file path
  struct Tex {
    ImTextureID tex{};
    bool tried = false;
  };
  std::map<std::wstring, Tex> textures;
  std::wstring path;
  std::filesystem::file_time_type mtime{};
  double last_check = -10;
  std::string on_load;
  std::vector<RxTab> tabs;
  std::map<std::string, RxState> state;  // by control id
  std::map<std::string, std::string> cache;  // "<id>|<what>" -> value from a get* callback
  std::map<std::string, bool> tab_visible;   // stable bools for Tab::visible
  bool host_synced = false;  // onLoad and the get* requests were sent to the current host
  bool dirty = false;        // rebuild the ribbon (a visibility changed)
  std::string error;
  int built_tabs = 0;
};

Rx& X() {
  static Rx x;
  return x;
}

// imageMso names -> our Fluent icons (names only, no Office images: hard rule 10)
const char* IconForMso(const std::string& n) {
  struct M {
    const char* mso;
    const char* icon;
  };
  static const M kMap[] = {
      {"FileSave", ui::icon::Save}, {"FileOpen", ui::icon::Open}, {"FileNew", ui::icon::New}, {"FileSaveAs", ui::icon::SaveAs}, {"FilePrint", ui::icon::Print},
      {"FilePrintQuick", ui::icon::Print}, {"FilePrintPreview", ui::icon::Print}, {"Copy", ui::icon::Copy}, {"Cut", ui::icon::Cut}, {"Paste", ui::icon::Paste},
      {"Undo", ui::icon::Undo}, {"Redo", ui::icon::Redo}, {"Bold", ui::icon::Bold}, {"Italic", ui::icon::Italic}, {"Underline", ui::icon::Underline},
      {"Refresh", ui::icon::Refresh}, {"RefreshAll", ui::icon::Refresh}, {"Recalculate", ui::icon::Calculator}, {"CalculateNow", ui::icon::Calculator},
      {"Calculator", ui::icon::Calculator}, {"AutoSum", ui::icon::AutoSum}, {"Filter", ui::icon::Filter}, {"SortAscendingExcel", ui::icon::SortFilter},
      {"SortDescendingExcel", ui::icon::SortFilter}, {"SortDialog", ui::icon::SortFilter}, {"TableInsert", ui::icon::Table}, {"TableInsertExcel", ui::icon::Table},
      {"ChartInsert", ui::icon::Chart}, {"ChartTypeColumnInsertGallery", ui::icon::ChartColumn}, {"ChartTypeLineInsertGallery", ui::icon::ChartLine},
      {"ChartTypePieInsertGallery", ui::icon::ChartPie}, {"PictureInsertFromFile", ui::icon::Picture}, {"ShapesInsertGallery", ui::icon::Shapes},
      {"TextBoxInsert", ui::icon::TextBox}, {"HyperlinkInsert", ui::icon::Link}, {"ReviewNewComment", ui::icon::Comment}, {"MacroPlay", ui::icon::Play},
      {"MacrosDialog", ui::icon::Play}, {"PlayMacro", ui::icon::Play}, {"VisualBasic", ui::icon::Code}, {"ViewCode", ui::icon::Code}, {"Delete", ui::icon::Dismiss},
      {"Cancel", ui::icon::Dismiss}, {"Clear", ui::icon::Clear}, {"ClearAll", ui::icon::Clear}, {"FindDialog", ui::icon::Search}, {"ZoomDialog", ui::icon::Zoom},
      {"Zoom100", ui::icon::Zoom100}, {"Info", ui::icon::Info}, {"Help", ui::icon::Info}, {"Properties", ui::icon::Properties}, {"PropertySheet", ui::icon::Properties},
      {"ControlsGallery", ui::icon::ControlsGroup}, {"DesignMode", ui::icon::DesignMode}, {"ConditionalFormattingMenu", ui::icon::CondFormat},
      {"FormatAsTableGallery", ui::icon::FormatAsTable}, {"DataValidation", ui::icon::Validation}, {"NameManager", ui::icon::NameManager},
      {"ReviewProtectSheet", ui::icon::ProtectSheet}, {"ReviewProtectWorkbook", ui::icon::ProtectWorkbook}, {"PivotTableInsert", ui::icon::PivotTable},
      {"PrintAreaMenu", ui::icon::PrintArea}, {"PageOrientationGallery", ui::icon::Orientation}, {"PageMarginsGallery", ui::icon::Margins},
      {"GroupFont", ui::icon::FontGroup}, {"FontColorPicker", ui::icon::FontColor}, {"CellFillColorPicker", ui::icon::FillColor},
      {"BordersGallery", ui::icon::BorderAll}, {"MergeCenter", ui::icon::Merge}, {"WrapText", ui::icon::WrapText}, {"Check", ui::icon::Check},
      {"AcceptInvitation", ui::icon::Check}, {"HappyFace", ui::icon::Play}, {"Grid", ui::icon::Grid}, {"Console", ui::icon::Console},
      {"CheckBoxInsert", ui::icon::CheckBox}, {"OptionButtonInsert", ui::icon::OptionButton}, {"ListBoxInsert", ui::icon::ListBox},
      {"ComboBoxInsert", ui::icon::ComboBox}, {"ButtonInsert", ui::icon::Button}, {"WindowsArrangeAll", ui::icon::WindowGroup}, {"FreezePanes", ui::icon::Freeze},
  };
  for (const M& m : kMap) {
    if (n == m.mso) return m.icon;
  }
  return n.empty() ? nullptr : ui::icon::Play;
}

std::string Attr(const pugi::xml_node& n, const char* a) { return n.attribute(a).as_string(); }

bool BoolAttr(const pugi::xml_node& n, const char* a, bool def) {
  const pugi::xml_attribute at = n.attribute(a);
  if (!at) return def;
  const std::string v = at.as_string();
  return v == "true" || v == "1";
}

RxControl ParseControl(const pugi::xml_node& n) {
  RxControl c;
  c.type = n.name();
  if (const char* colon = std::strchr(c.type.c_str(), ':')) c.type = colon + 1;  // a namespace prefix
  c.id = Attr(n, "id");
  c.id_mso = Attr(n, "idMso");
  if (c.id.empty()) c.id = !c.id_mso.empty() ? c.id_mso : Attr(n, "idQ");
  c.tag = Attr(n, "tag");
  c.label = Attr(n, "label");
  c.image_mso = Attr(n, "imageMso");
  c.image = Attr(n, "image");
  c.get_image = Attr(n, "getImage");
  c.screentip = Attr(n, "screentip");
  c.supertip = Attr(n, "supertip");
  c.large = Attr(n, "size") == "large";
  c.enabled = BoolAttr(n, "enabled", true);
  c.visible = BoolAttr(n, "visible", true);
  c.on_action = Attr(n, "onAction");
  c.on_change = Attr(n, "onChange");
  c.get_label = Attr(n, "getLabel");
  c.get_enabled = Attr(n, "getEnabled");
  c.get_visible = Attr(n, "getVisible");
  c.get_pressed = Attr(n, "getPressed");
  c.get_text = Attr(n, "getText");
  c.get_selected_index = Attr(n, "getSelectedItemIndex");
  c.get_item_count = Attr(n, "getItemCount");
  c.get_item_label = Attr(n, "getItemLabel");
  c.width_chars = static_cast<int>(std::string(Attr(n, "sizeString")).size());
  for (pugi::xml_node ch : n.children()) {
    std::string t = ch.name();
    if (const size_t colon = t.find(':'); colon != std::string::npos) t = t.substr(colon + 1);
    if (t == "item") c.items.push_back(RxItem{Attr(ch, "id"), Attr(ch, "label")});
    else if (ch.type() == pugi::node_element) c.children.push_back(ParseControl(ch));
  }
  return c;
}

// box / buttonGroup are layout containers: their children join the group
void CollectControls(const pugi::xml_node& parent, std::vector<RxControl>* out) {
  for (pugi::xml_node ch : parent.children()) {
    if (ch.type() != pugi::node_element) continue;
    std::string t = ch.name();
    if (const size_t colon = t.find(':'); colon != std::string::npos) t = t.substr(colon + 1);
    if (t == "box" || t == "buttonGroup") {
      CollectControls(ch, out);
    } else if (t != "dialogBoxLauncher") {
      out->push_back(ParseControl(ch));
    }
  }
}

bool LoadFile(const std::wstring& path, std::string* err) {
  Rx& x = X();
  pugi::xml_document doc;
  const pugi::xml_parse_result res = doc.load_file(path.c_str());
  x.tabs.clear();
  x.on_load.clear();
  if (!res) {
    *err = std::string("customUI.xml: ") + res.description() + " (offset " + std::to_string(res.offset) + ")";
    return false;
  }
  auto child = [](const pugi::xml_node& n, const char* name) {
    for (pugi::xml_node c : n.children()) {
      std::string t = c.name();
      if (const size_t colon = t.find(':'); colon != std::string::npos) t = t.substr(colon + 1);
      if (t == name) return c;
    }
    return pugi::xml_node();
  };
  const pugi::xml_node root = child(doc, "customUI");
  if (!root) {
    *err = "customUI.xml: the root element must be <customUI>";
    return false;
  }
  x.on_load = Attr(root, "onLoad");
  const pugi::xml_node tabs = child(child(root, "ribbon"), "tabs");
  for (pugi::xml_node tn : tabs.children()) {
    if (tn.type() != pugi::node_element) continue;
    RxTab t;
    t.id = Attr(tn, "id");
    t.id_mso = Attr(tn, "idMso");
    t.label = Attr(tn, "label");
    t.visible = BoolAttr(tn, "visible", true);
    t.get_visible = Attr(tn, "getVisible");
    if (t.id.empty()) t.id = !t.id_mso.empty() ? t.id_mso : "tab" + std::to_string(x.tabs.size() + 1);
    for (pugi::xml_node gn : tn.children()) {
      if (gn.type() != pugi::node_element) continue;
      RxGroup g;
      g.id = Attr(gn, "id");
      g.label = Attr(gn, "label");
      g.visible = BoolAttr(gn, "visible", true);
      g.get_visible = Attr(gn, "getVisible");
      CollectControls(gn, &g.controls);
      if (pugi::xml_node l = child(gn, "dialogBoxLauncher")) g.launcher_action = Attr(child(l, "button"), "onAction");
      t.groups.push_back(std::move(g));
    }
    x.tabs.push_back(std::move(t));
  }
  return true;
}

// ---- talking to the Python host ----
void Notify(AppState& s, const char* op, const std::string& callback, const RxControl* c, MV args = MV::Array(), const std::string& key = {}) {
  if (callback.empty() || !s.python || !s.python->HostReady()) return;
  MV p = MV::Map();
  p.Set("op", MV::Str(op));
  p.Set("callback", MV::Str(callback));
  p.Set("control", MV::Str(c ? c->id : std::string()));
  p.Set("tag", MV::Str(c ? c->tag : std::string()));
  p.Set("args", std::move(args));
  if (!key.empty()) p.Set("key", MV::Str(key));
  s.python->RibbonNotify(p);
}

void RequestGet(AppState& s, const RxControl& c, const std::string& callback, const char* what, MV args = MV::Array()) {
  if (callback.empty()) return;
  Notify(s, "get", callback, &c, std::move(args), c.id + "|" + what);
}

const std::string* Cached(const std::string& id, const char* what) {
  auto it = X().cache.find(id + "|" + what);
  return it == X().cache.end() ? nullptr : &it->second;
}

bool CachedBool(const std::string& id, const char* what, bool def) {
  const std::string* v = Cached(id, what);
  return v ? (*v == "1" || *v == "true" || *v == "True") : def;
}

void ForEachControl(const std::function<void(const RxControl&)>& fn) {
  std::function<void(const RxControl&)> walk = [&](const RxControl& c) {
    fn(c);
    for (const auto& ch : c.children) walk(ch);
  };
  for (const auto& t : X().tabs) {
    for (const auto& g : t.groups) {
      for (const auto& c : g.controls) walk(c);
    }
  }
}

void RequestAll(AppState& s, const std::string& only = {}) {
  ForEachControl([&](const RxControl& c) {
    if (!only.empty() && c.id != only) return;
    RequestGet(s, c, c.get_label, "label");
    RequestGet(s, c, c.get_enabled, "enabled");
    RequestGet(s, c, c.get_visible, "visible");
    RequestGet(s, c, c.get_pressed, "pressed");
    RequestGet(s, c, c.get_text, "text");
    RequestGet(s, c, c.get_selected_index, "index");
    RequestGet(s, c, c.get_item_count, "count");
    RequestGet(s, c, c.get_image, "image");
  });
  for (const auto& t : X().tabs) {
    if (!t.get_visible.empty() && (only.empty() || only == t.id)) {
      RxControl tmp;
      tmp.id = t.id;
      RequestGet(s, tmp, t.get_visible, "visible");
    }
    for (const auto& g : t.groups) {
      if (!g.get_visible.empty() && (only.empty() || only == g.id)) {
        RxControl tmp;
        tmp.id = g.id;
        RequestGet(s, tmp, g.get_visible, "visible");
      }
    }
  }
}

// image="name": a picture file in the project folder (name, name.png, images\name.png, customUI\images\name.png);
// getImage returns such a name or a full path
ImTextureID ImageFor(AppState& s, const RxControl& c) {
  Rx& x = X();
  std::string name = c.image;
  if (const std::string* v = Cached(c.id, "image"); v && !v->empty()) name = *v;
  if (name.empty()) return ImTextureID{};
  const std::filesystem::path base = std::filesystem::path(x.path).parent_path();
  const std::filesystem::path want = std::filesystem::path(FromUtf8(name));
  std::vector<std::filesystem::path> tries;
  if (want.is_absolute()) tries.push_back(want);
  for (const auto& dir : {base, base / L"images", base / L"customUI" / L"images"}) {
    tries.push_back(dir / want);
    for (const wchar_t* ext : {L".png", L".jpg", L".bmp", L".gif", L".ico"}) tries.push_back(dir / (want.wstring() + ext));
  }
  for (const auto& p : tries) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) continue;
    auto& t = x.textures[p.wstring()];
    if (!t.tried) {
      t.tried = true;
      std::ifstream f(p, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      t.tex = TextureFromImage(bytes);
      if (!t.tex) s.SetStatus(std::string(TR("This picture can't be read.")) + " " + ToUtf8(p.wstring()));
    }
    return t.tex;
  }
  return ImTextureID{};
}

void ReleaseImages() {
  for (auto& [p, t] : X().textures) {
    if (t.tex) ReleaseTexture(t.tex);
  }
  X().textures.clear();
}

// ---- building the ribbon ----
std::string CmdId(const RxControl& c) { return "rx." + c.id; }

std::string LabelOf(const RxControl& c) {
  const std::string* v = Cached(c.id, "label");
  return v ? *v : c.label;
}

bool VisibleOf(const RxControl& c) { return CachedBool(c.id, "visible", c.visible); }

void RegisterCommand(AppState& s, const RxControl& c) {
  // a built-in control (idMso) uses YCGrid's own command when there is one
  if (!c.id_mso.empty() && ui::Commands().Find(c.id_mso)) return;
  ui::Command cmd;
  cmd.id = CmdId(c);
  cmd.label = LabelOf(c);
  cmd.icon = IconForMso(c.image_mso);
  cmd.tip_title = c.screentip;
  cmd.tip_desc = c.supertip;
  cmd.toggle = c.type == "toggleButton" || c.type == "checkBox";
  cmd.enabled = CachedBool(c.id, "enabled", c.enabled);
  RxState& st = X().state[c.id];
  st.def = &c;
  if (!Cached(c.id, "pressed")) X().cache[c.id + "|pressed"] = st.pressed ? "1" : "0";
  cmd.pressed = st.pressed;
  const std::string id = c.id;
  cmd.execute = [&s, id](ui::Command& self) {
    Rx& x = X();
    auto it = x.state.find(id);
    if (it == x.state.end() || !it->second.def) return;
    RxState& st2 = it->second;
    const RxControl& d = *st2.def;
    MV args = MV::Array();
    if (d.type == "toggleButton" || d.type == "checkBox") {
      // the cache holds the state before the click (a ribbon check box has already flipped its bool)
      st2.pressed = !CachedBool(id, "pressed", false);
      self.pressed = st2.pressed;
      x.cache[id + "|pressed"] = st2.pressed ? "1" : "0";
      args.Push(MV::Bool(st2.pressed));
    }
    if (d.on_action.empty()) {
      s.SetStatus(TR("This ribbon control has no onAction: ") + id);
      return;
    }
    if (!s.python || !s.python->HostReady()) {
      s.SetStatus(TR("Python is not ready yet: ") + d.on_action);
      return;
    }
    Notify(s, "action", d.on_action, &d, std::move(args));
  };
  ui::Commands().Add(std::move(cmd));
}

// gallery / dropDown item i: a command in the drop-down menu
void RegisterItemCommand(AppState& s, const RxControl& c, size_t i, const RxItem& item) {
  ui::Command cmd;
  cmd.id = CmdId(c) + "#" + std::to_string(i);
  cmd.label = item.label.empty() ? item.id : item.label;
  const std::string id = c.id;
  const std::string item_id = item.id;
  const int index = static_cast<int>(i);
  cmd.execute = [&s, id, item_id, index](ui::Command&) {
    auto it = X().state.find(id);
    if (it == X().state.end() || !it->second.def) return;
    RxState& st = it->second;
    st.index = index;
    MV args = MV::Array();
    args.Push(MV::Str(item_id));
    args.Push(MV::Int(index));
    Notify(s, "action", st.def->on_action, st.def, std::move(args));
  };
  ui::Commands().Add(std::move(cmd));
}

std::vector<RxItem> ItemsOf(const RxControl& c) {
  if (c.get_item_count.empty()) return c.items;
  std::vector<RxItem> out;
  const std::string* n = Cached(c.id, "count");
  const int count = n ? std::atoi(n->c_str()) : 0;
  for (int i = 0; i < count && i < 1000; ++i) {
    const std::string* l = Cached(c.id, ("item" + std::to_string(i)).c_str());
    out.push_back(RxItem{"item" + std::to_string(i), l ? *l : std::string()});
  }
  return out;
}

// dropDown, comboBox, editBox, labelControl: drawn in a stack row
void DrawCustom(AppState& s, const std::string& id, const ImRect& r) {
  auto it = X().state.find(id);
  if (it == X().state.end() || !it->second.def) return;
  RxState& st = it->second;
  const RxControl& c = *st.def;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool enabled = CachedBool(c.id, "enabled", c.enabled);
  const std::string label = LabelOf(c);
  const ImU32 text_col = enabled ? IM_COL32(0x26, 0x26, 0x26, 255) : IM_COL32(0xA0, 0xA0, 0xA0, 255);
  float x = r.Min.x + S(4);
  if (const char* icon = IconForMso(c.image_mso)) {
    ui::ribbon::DrawRibbonIcon(dl, icon, ImVec2(x + S(8), r.GetCenter().y), ui::F().icon_small, text_col);
    x += S(20);
  }
  if (!label.empty()) {
    dl->AddText(ImVec2(x, r.GetCenter().y - ImGui::GetFontSize() * 0.5f), text_col, label.c_str());
    x += ImGui::CalcTextSize(label.c_str()).x + S(6);
  }
  if (c.type == "labelControl") return;
  const ImRect box(ImVec2(x, r.Min.y + S(2)), ImVec2(r.Max.x - S(2), r.Max.y - S(2)));
  ImGui::PushID(id.c_str());
  ImGui::BeginDisabled(!enabled);
  const std::vector<RxItem> items = ItemsOf(c);
  if (const std::string* t = Cached(c.id, "text"); t && !st.editing) st.text = *t;
  if (const std::string* ix = Cached(c.id, "index")) st.index = std::atoi(ix->c_str());
  ImGui::SetCursorScreenPos(box.Min);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), std::max(0.0f, (box.GetHeight() - ImGui::GetFontSize()) * 0.5f)));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
  ImGui::SetNextItemWidth(box.GetWidth());
  if (c.type == "dropDown") {
    const std::string cur = st.index >= 0 && st.index < static_cast<int>(items.size()) ? items[static_cast<size_t>(st.index)].label : std::string();
    if (ImGui::BeginCombo("##dd", cur.c_str())) {
      for (size_t i = 0; i < items.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(items[i].label.empty() ? " " : items[i].label.c_str(), static_cast<int>(i) == st.index)) {
          st.index = static_cast<int>(i);
          X().cache[c.id + "|index"] = std::to_string(i);
          MV args = MV::Array();
          args.Push(MV::Str(items[i].id));
          args.Push(MV::Int(static_cast<int64_t>(i)));
          Notify(s, "action", c.on_action, &c, std::move(args));
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
  } else {
    // comboBox / editBox: typing commits with Enter or leaving the box (onChange)
    if (!st.editing) std::snprintf(st.buf, sizeof(st.buf), "%s", st.text.c_str());
    const float arrow = c.type == "comboBox" ? ImGui::GetFrameHeight() : 0.0f;
    ImGui::SetNextItemWidth(box.GetWidth() - arrow);
    ImGui::InputText("##eb", st.buf, sizeof(st.buf));
    st.editing = ImGui::IsItemActive();
    auto commit = [&](const std::string& text) {
      st.text = text;
      X().cache[c.id + "|text"] = text;
      MV args = MV::Array();
      args.Push(MV::Str(text));
      Notify(s, "action", c.on_change, &c, std::move(args));
    };
    if (ImGui::IsItemDeactivatedAfterEdit()) commit(st.buf);
    if (c.type == "comboBox") {
      ImGui::SameLine(0, 0);
      if (ImGui::ArrowButton("##arrow", ImGuiDir_Down)) ImGui::OpenPopup("##items");
      if (ImGui::BeginPopup("##items")) {
        for (size_t i = 0; i < items.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          if (ImGui::Selectable(items[i].label.empty() ? " " : items[i].label.c_str(), items[i].label == st.text)) commit(items[i].label);
          ImGui::PopID();
        }
        ImGui::EndPopup();
      }
    }
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::EndDisabled();
  ImGui::PopID();
}

float CustomWidth(const RxControl& c) {
  ImGui::PushFont(ui::F().ui, ui::F().ui_size);
  float w = ImGui::CalcTextSize(c.label.c_str()).x / S(1) + (c.image_mso.empty() ? 0.0f : 20.0f) + 10;
  ImGui::PopFont();
  if (c.type == "labelControl") return w;
  const float chars = c.width_chars > 0 ? static_cast<float>(c.width_chars) : 12.0f;
  return w + std::clamp(chars * 7.0f + 12.0f, 60.0f, 260.0f) + (c.type == "comboBox" ? 20.0f : 0.0f);
}

ui::ribbon::Item MakeItem(AppState& s, const RxControl& c, bool* is_large) {
  using K = ui::ribbon::ItemKind;
  ui::ribbon::Item it;
  *is_large = c.large;
  const bool builtin = !c.id_mso.empty() && ui::Commands().Find(c.id_mso);
  it.cmd = builtin ? c.id_mso : CmdId(c);
  if (c.type == "dropDown" || c.type == "comboBox" || c.type == "editBox" || c.type == "labelControl") {
    it.kind = K::Custom;
    it.combo_width = CustomWidth(c);
    const std::string id = c.id;
    it.draw = [&s, id](const ImRect& r) { DrawCustom(s, id, r); };
    *is_large = false;
    X().state[c.id].def = &c;
    return it;
  }
  if (!builtin) RegisterCommand(s, c);
  if (c.type == "toggleButton") it.kind = K::Toggle;
  if (c.type == "checkBox") {
    it.kind = K::Check;
    it.check = &X().state[c.id].pressed;
    *is_large = false;
  }
  if (c.type == "menu" || c.type == "gallery") it.kind = K::Menu;
  if (c.type == "gallery") {
    const std::vector<RxItem> items = ItemsOf(c);
    for (size_t i = 0; i < items.size(); ++i) {
      RegisterItemCommand(s, c, i, items[i]);
      it.menu.push_back(CmdId(c) + "#" + std::to_string(i));
    }
  }
  // menus: their buttons, flattened (nested menus between separators)
  std::function<void(const RxControl&)> add_menu = [&](const RxControl& m) {
    for (const auto& ch : m.children) {
      if (ch.type == "menuSeparator") {
        it.menu.push_back("-");
      } else if (ch.type == "menu") {
        if (!it.menu.empty() && it.menu.back() != "-") it.menu.push_back("-");
        add_menu(ch);
        it.menu.push_back("-");
      } else if (VisibleOf(ch)) {
        bool ignored = false;
        const ui::ribbon::Item sub = MakeItem(s, ch, &ignored);
        it.menu.push_back(sub.cmd);
      }
    }
  };
  if (c.type == "menu") add_menu(c);
  if (c.type == "splitButton") {
    // <button> (or <toggleButton>) is the body, <menu> the drop-down
    it.kind = K::Split;
    for (const auto& ch : c.children) {
      if (ch.type == "button" || ch.type == "toggleButton") {
        bool ignored = false;
        it.cmd = MakeItem(s, ch, &ignored).cmd;
        if (ch.large) *is_large = true;
      } else if (ch.type == "menu") {
        add_menu(ch);
      }
    }
  }
  it.icon = IconForMso(c.image_mso);
  if (!c.image.empty() || !c.get_image.empty()) {
    const std::string id = c.id;
    it.image = [&s, id]() -> ImTextureID {
      auto st = X().state.find(id);
      return st != X().state.end() && st->second.def ? ImageFor(s, *st->second.def) : ImTextureID{};
    };
    X().state[c.id].def = &c;
    if (!it.icon) it.icon = ui::icon::Play;
  }
  return it;
}

void BuildRibbon(AppState& s) {
  Rx& x = X();
  ui::ribbon::BuildDefaultTabs(&s.show_gridlines, &s.show_headings, &s.show_formula_bar);
  UpdateFunctionLibraryMenu();  // Formulas > Recently Used
  auto& tabs = ui::ribbon::Tabs();
  // remove the old custom commands
  {
    std::vector<std::string> old;
    for (const auto& c : ui::Commands().all()) {
      if (c->id.rfind("rx.", 0) == 0) old.push_back(c->id);
    }
    for (const auto& id : old) {
      if (ui::Command* c = ui::Commands().Find(id)) {
        c->execute = nullptr;
        c->label = id;
      }
    }
  }
  static const struct {
    const char* mso;
    const char* label;
  } kBuiltin[] = {{"TabHome", "Home"}, {"TabInsert", "Insert"}, {"TabPageLayoutExcel", "Page Layout"}, {"TabFormulas", "Formulas"}, {"TabData", "Data"},
                  {"TabReview", "Review"}, {"TabView", "View"}, {"TabDeveloper", "Developer"}};
  for (const RxTab& t : x.tabs) {
    if (!CachedBool(t.id, "visible", t.visible)) continue;
    ui::ribbon::Tab* target = nullptr;
    if (!t.id_mso.empty()) {
      for (const auto& b : kBuiltin) {
        if (t.id_mso != b.mso) continue;
        for (auto& bt : tabs) {
          if (bt.label == i18n::Tr(b.label)) target = &bt;
        }
      }
      if (!target) continue;  // an unknown built-in tab
    } else {
      ui::ribbon::Tab nt;
      nt.label = t.label.empty() ? t.id : t.label;
      tabs.push_back(std::move(nt));
      target = &tabs.back();
    }
    for (const RxGroup& g : t.groups) {
      if (!CachedBool(g.id, "visible", g.visible)) continue;
      ui::ribbon::Group gr;
      gr.label = g.label;
      gr.icon = ui::icon::ControlsGroup;
      ui::ribbon::Block stack;
      stack.kind = ui::ribbon::BlockKind::Stack;
      auto flush = [&]() {
        if (!stack.items.empty()) gr.blocks.push_back(std::move(stack));
        stack = {};
        stack.kind = ui::ribbon::BlockKind::Stack;
      };
      for (const RxControl& c : g.controls) {
        if (c.type == "separator") {
          flush();
          continue;
        }
        if (!VisibleOf(c)) continue;
        bool large = false;
        ui::ribbon::Item it = MakeItem(s, c, &large);
        if (gr.icon == ui::icon::ControlsGroup && it.icon) gr.icon = it.icon;
        if (large) {
          flush();
          ui::ribbon::Block b;
          b.kind = ui::ribbon::BlockKind::Large;
          b.items.push_back(std::move(it));
          gr.blocks.push_back(std::move(b));
        } else {
          stack.items.push_back(std::move(it));
        }
      }
      flush();
      if (!g.launcher_action.empty()) {
        RxControl l;
        l.id = g.id + ".launcher";
        l.type = "button";
        l.on_action = g.launcher_action;
        l.label = g.label;
        // the definition must outlive the command: keep it in the state map
        RxState& st = x.state[l.id];
        static std::map<std::string, std::unique_ptr<RxControl>> launchers;
        launchers[l.id] = std::make_unique<RxControl>(l);
        st.def = launchers[l.id].get();
        RegisterCommand(s, *st.def);
        gr.launcher = CmdId(l);
      }
      target->groups.push_back(std::move(gr));
    }
  }
  x.built_tabs = static_cast<int>(x.tabs.size());
  ui::ribbon::InvalidateLayout();
  x.dirty = false;
}

}  // namespace

void UpdateCustomRibbon(AppState& s) {
  Rx& x = X();
  const double now = ImGui::GetTime();
  if (now - x.last_check > 1.0) {
    x.last_check = now;
    // customUI14.xml (Excel 2010+) or customUI.xml in the project folder
    std::wstring path;
    for (const wchar_t* name : {L"customUI14.xml", L"customUI.xml"}) {
      const std::filesystem::path p = std::filesystem::path(s.app_dir) / name;
      std::error_code ec;
      if (!s.app_dir.empty() && std::filesystem::exists(p, ec)) {
        path = p.wstring();
        break;
      }
    }
    std::error_code ec;
    const auto mtime = path.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
    if (path != x.path || mtime != x.mtime) {
      const bool had = !x.tabs.empty() || !x.path.empty();
      x.path = path;
      x.mtime = mtime;
      x.cache.clear();
      x.state.clear();
      x.error.clear();
      ReleaseImages();
      if (!path.empty() && !LoadFile(path, &x.error)) {
        s.SetStatus(x.error, 10);
        if (s.python) s.python->LogSystem(x.error);
      }
      if (!path.empty() || had) BuildRibbon(s);
      x.host_synced = false;
    }
  }
  const bool ready = s.python && s.python->HostReady();
  if (!ready) {
    x.host_synced = false;
  } else if (!x.host_synced && !x.path.empty()) {
    x.host_synced = true;
    if (!x.on_load.empty()) Notify(s, "load", x.on_load, nullptr);
    RequestAll(s);
  }
  if (x.dirty) BuildRibbon(s);
  // cached labels / enabled / pressed states onto the commands
  for (auto& [id, st] : x.state) {
    if (!st.def) continue;
    ui::Command* c = ui::Commands().Find(CmdId(*st.def));
    if (!c) continue;
    c->label = LabelOf(*st.def);
    c->enabled = CachedBool(id, "enabled", st.def->enabled);
    if (const std::string* p = Cached(id, "pressed")) st.pressed = *p == "1" || *p == "true" || *p == "True";
    c->pressed = st.pressed;
  }
}

// a get* result from Python (UI thread)
void SetRibbonValue(AppState& s, const std::string& key, const std::string& value) {
  Rx& x = X();
  auto it = x.cache.find(key);
  const bool changed = it == x.cache.end() || it->second != value;
  x.cache[key] = value;
  if (!changed) return;
  const size_t bar = key.rfind('|');
  const std::string what = bar == std::string::npos ? std::string() : key.substr(bar + 1);
  // visibility and item lists change the ribbon's structure
  if (what == "visible") x.dirty = true;
  if (what == "image") {
    // a new picture name: load it on the next draw
    for (auto& [p, t] : x.textures) t.tried = t.tex != ImTextureID{} ? t.tried : false;
  }
  if (what == "count") {
    const std::string id = key.substr(0, bar);
    ForEachControl([&](const RxControl& c) {
      if (c.id != id || c.get_item_label.empty()) return;
      const int n = std::atoi(value.c_str());
      for (int i = 0; i < n && i < 1000; ++i) {
        MV args = MV::Array();
        args.Push(MV::Int(i));
        RequestGet(s, c, c.get_item_label, ("item" + std::to_string(i)).c_str(), std::move(args));
      }
      if (c.type == "gallery") x.dirty = true;
    });
  }
}

// IRibbonUI.Invalidate / InvalidateControl
void InvalidateRibbon(AppState& s, const std::string& control) {
  // only values that come from a get* callback are dropped; a toggle's own state stays
  Rx& x = X();
  std::set<std::string> keys;
  ForEachControl([&](const RxControl& c) {
    if (!control.empty() && c.id != control) return;
    const std::pair<const std::string*, const char*> gets[] = {{&c.get_label, "label"},  {&c.get_enabled, "enabled"}, {&c.get_visible, "visible"},
                                                               {&c.get_pressed, "pressed"}, {&c.get_text, "text"},     {&c.get_selected_index, "index"},
                                                               {&c.get_item_count, "count"}};
    for (const auto& [cb, what] : gets) {
      if (!cb->empty()) keys.insert(c.id + "|" + what);
    }
    if (!c.get_item_label.empty()) {
      for (const auto& [k, v] : x.cache) {
        if (k.rfind(c.id + "|item", 0) == 0) keys.insert(k);
      }
    }
  });
  for (const auto& k : keys) x.cache.erase(k);
  RequestAll(s, control);
}

bool ActivateRibbonTab(const std::string& id) {
  std::string label;
  for (const auto& t : X().tabs) {
    if (t.id == id && t.id_mso.empty()) label = t.label.empty() ? t.id : t.label;
  }
  static const struct {
    const char* mso;
    const char* label;
  } kBuiltin[] = {{"TabHome", "Home"}, {"TabInsert", "Insert"}, {"TabPageLayoutExcel", "Page Layout"}, {"TabFormulas", "Formulas"}, {"TabData", "Data"},
                  {"TabReview", "Review"}, {"TabView", "View"}, {"TabDeveloper", "Developer"}};
  for (const auto& b : kBuiltin) {
    if (id == b.mso) label = i18n::Tr(b.label);
  }
  auto& tabs = ui::ribbon::Tabs();
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (!label.empty() && tabs[i].label == label && !tabs[i].backstage) {
      ui::ribbon::St().selected_tab = static_cast<int>(i);
      return true;
    }
  }
  return false;
}

MV RibbonInfo() {
  Rx& x = X();
  MV o = MV::Map();
  o.Set("path", MV::Str(ToUtf8(x.path)));
  o.Set("error", MV::Str(x.error));
  o.Set("tabs", MV::Int(static_cast<int64_t>(x.tabs.size())));
  MV ctl = MV::Array();
  ForEachControl([&](const RxControl& c) {
    MV m = MV::Map();
    m.Set("id", MV::Str(c.id));
    m.Set("type", MV::Str(c.type));
    m.Set("label", MV::Str(LabelOf(c)));
    m.Set("enabled", MV::Bool(CachedBool(c.id, "enabled", c.enabled)));
    m.Set("visible", MV::Bool(VisibleOf(c)));
    auto st = x.state.find(c.id);
    m.Set("pressed", MV::Bool(st != x.state.end() && st->second.pressed));
    m.Set("text", MV::Str(st != x.state.end() ? st->second.text : std::string()));
    m.Set("command", MV::Str(c.id_mso.empty() || !ui::Commands().Find(c.id_mso) ? CmdId(c) : c.id_mso));
    ctl.Push(std::move(m));
  });
  o.Set("controls", std::move(ctl));
  return o;
}

}  // namespace yc::app
