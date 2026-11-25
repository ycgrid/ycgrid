// SPDX-License-Identifier: Apache-2.0
// links_ui.cpp - links to other workbooks: reading their values (when the workbook opens and when a source is saved),
// Data > Edit Links (Update Values, Change Source, Open Source, Break Link)
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "engine/external.h"
#include "engine/tile_iter.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "io/csv.h"
#include "io/xlsx.h"
#include "io/ycg.h"

#include "imgui.h"

#include <windows.h>
#include <commdlg.h>

#include <ctime>
#include <filesystem>
#include <map>

namespace yc::app {

using engine::Document;
using ui::S;
namespace fs = std::filesystem;

namespace {

std::map<std::string, fs::file_time_type> g_seen;  // source path -> the write time last read
std::string g_seen_book;                            // the workbook the times belong to
double g_next_poll = 0;
int g_sel = 0;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// where a source is: its path, else the same name next to this workbook
std::wstring ResolveSource(const AppState& s, const std::string& path) {
  std::error_code ec;
  const std::wstring w = FromUtf8(path);
  if (!w.empty() && fs::exists(w, ec)) return fs::absolute(w, ec).wstring();
  const std::wstring name = fs::path(w).filename().wstring();
  if (!s.file_path.empty()) {
    const fs::path here = fs::path(s.file_path).parent_path() / name;
    if (fs::exists(here, ec)) return here.wstring();
  }
  return {};
}

}  // namespace

// read a link's source and hand its values to the engine; false with *err
bool UpdateLink(AppState& s, size_t link, std::string* err) {
  if (!s.snap || link >= s.snap->wb.external_links->size()) return false;
  const model::ExternalLink l = (*s.snap->wb.external_links)[link];
  if (l.path.empty()) return false;  // a broken link
  const std::wstring src = ResolveSource(s, l.path);
  std::string error;
  std::vector<std::map<uint64_t, core::Value>> values(l.sheets.size());
  model::Workbook wb;
  if (src.empty()) {
    error = TR("The source wasn't found: ") + l.path;
  } else {
    bool ok = false;
    try {
      if (io::IsXlsxPath(src)) {
        ok = io::ImportXlsx(src, &wb, &error, nullptr);
      } else if (Lower(ToUtf8(fs::path(src).extension().wstring())) == ".csv") {
        ok = io::ImportCsv(src, &wb, &error);
        if (ok && !wb.sheets.empty()) wb.sheets[0].mut().name = ToUtf8(fs::path(src).stem().wstring());  // a CSV's one sheet has the file's name
      } else {
        io::LoadResult r = io::LoadYcg(src);
        ok = r.ok;
        if (r.needs_password) error = TR("The source is encrypted: open it in YCGrid to read it");
        else if (!r.ok) error = r.error;
        wb = std::move(r.wb);
      }
    } catch (const std::exception& e) {  // a crafted source (sizes that don't fit in memory): an error, not a crash
      ok = false;
      error = TR("The file can't be read: ") + std::string(e.what());
    }
    if (ok) {
      for (size_t si = 0; si < l.sheets.size(); ++si) {
        const int32_t idx = wb.FindSheet(l.sheets[si]);
        if (idx < 0) continue;
        const model::Sheet& sh = *wb.sheets[static_cast<size_t>(idx)];
        if (!sh.has_used) continue;
        size_t n = 0;
        engine::ForEachValueRowMajor(sh, sh.used, [&](uint32_t r, uint32_t c, const core::Value& v) {
          if (c < 0x10000) values[si][(uint64_t{r} << 16) | c] = v;
          return ++n < 4000000;
        });
      }
    }
  }
  std::error_code ec;
  if (!src.empty()) g_seen[l.path] = fs::last_write_time(src, ec);
  const std::string resolved = src.empty() ? std::string() : ToUtf8(src);
  const double now = static_cast<double>(std::time(nullptr));
  s.engine->Post([link, values = std::move(values), now, error, resolved, from = l.path](Document& d) mutable {
    d.SetExternalValues(link, std::move(values), now, error, resolved, &from);
  });
  if (err) *err = error;
  return error.empty();
}

bool UpdateAllLinks(AppState& s, std::string* first_err) {
  bool all = true;
  const size_t n = s.snap ? s.snap->wb.external_links->size() : 0;
  for (size_t i = 0; i < n; ++i) {
    if ((*s.snap->wb.external_links)[i].path.empty()) continue;
    std::string e;
    if (!UpdateLink(s, i, &e)) {
      all = false;
      if (first_err && first_err->empty()) *first_err = e;
    }
  }
  return all;
}

void UpdateLinksUi(AppState& s) {
  if (!s.snap) return;
  const auto& links = *s.snap->wb.external_links;
  // another workbook (opened, or new): read its links' sources once
  const std::string book = ToUtf8(s.file_path);
  if (book != g_seen_book) {
    g_seen_book = book;
    g_seen.clear();
    if (!links.empty()) UpdateAllLinks(s, nullptr);
    return;
  }
  // a source saved again (also from another YCGrid window): read it again, like Excel's live links
  if (links.empty()) return;
  if (ImGui::GetTime() < g_next_poll) {
    ui::RequestRedrawIn(g_next_poll - ImGui::GetTime());  // an idle window still looks again
    return;
  }
  g_next_poll = ImGui::GetTime() + 2.0;
  for (size_t i = 0; i < links.size(); ++i) {
    if (links[i].path.empty()) continue;
    const std::wstring src = ResolveSource(s, links[i].path);
    if (src.empty()) continue;
    std::error_code ec;
    const auto t = fs::last_write_time(src, ec);
    auto it = g_seen.find(links[i].path);
    if (it == g_seen.end() || it->second != t) UpdateLink(s, i, nullptr);
  }
  ui::RequestRedrawIn(2.0);
}

void RegisterLinkCommands(AppState& s) {
  ui::Command c;
  c.id = "EditLinks";
  c.label = TR("Edit\nLinks");
  c.icon = ui::icon::Link;
  c.tip_desc = TR("See and update the workbooks this workbook's formulas refer to.");
  ui::Commands().Add(std::move(c)).execute = [&s](ui::Command&) { s.OpenDialog(Dialog::EditLinks); };
}

void DrawLinksDialog(AppState& s) {
  if (auto* c = ui::Commands().Find("EditLinks")) {
    bool any = false;
    if (s.snap) {
      for (const auto& l : *s.snap->wb.external_links) any = any || !l.path.empty();
    }
    c->enabled = any;
  }
  if (s.dialog != Dialog::EditLinks) return;
  const char* title = TR("Edit Links");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  // the list (560) and the buttons beside it (150), with the dialog's margins: a fitted width left no right margin
  ImGui::SetNextWindowSize(ImVec2(S(760), 0), ImGuiCond_Appearing);
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
  const auto& links = *s.snap->wb.external_links;
  std::vector<size_t> rows;
  for (size_t i = 0; i < links.size(); ++i) {
    if (!links[i].path.empty()) rows.push_back(i);
  }
  g_sel = std::clamp(g_sel, 0, std::max(0, static_cast<int>(rows.size()) - 1));
  if (ImGui::BeginTable("##links", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(S(560), S(260)))) {
    ImGui::TableSetupColumn(TR("Source"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(TR("Type"), ImGuiTableColumnFlags_WidthFixed, S(80));
    ImGui::TableSetupColumn(TR("Updated"), ImGuiTableColumnFlags_WidthFixed, S(130));
    ImGui::TableSetupColumn(TR("Status"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (size_t k = 0; k < rows.size(); ++k) {
      const auto& l = links[rows[k]];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushID(static_cast<int>(k));
      if (ImGui::Selectable(ToUtf8(fs::path(FromUtf8(l.path)).filename().wstring()).c_str(), g_sel == static_cast<int>(k), ImGuiSelectableFlags_SpanAllColumns))
        g_sel = static_cast<int>(k);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.path.c_str());
      ImGui::PopID();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(TR("Worksheet"));
      ImGui::TableSetColumnIndex(2);
      if (l.updated > 0) {
        const std::time_t tt = static_cast<std::time_t>(l.updated);
        std::tm lt{};
        localtime_s(&lt, &tt);
        char b[32];
        std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
        ImGui::TextUnformatted(b);
      } else {
        ImGui::TextDisabled("%s", TR("Never"));
      }
      ImGui::TableSetColumnIndex(3);
      if (l.error.empty()) ImGui::TextUnformatted(l.updated > 0 ? TR("OK") : TR("Unknown"));
      else ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.2f, 1), "%s", l.error.c_str());
    }
    ImGui::EndTable();
  }
  ImGui::SameLine();
  ImGui::BeginGroup();
  const bool has = !rows.empty();
  const size_t cur = has ? rows[static_cast<size_t>(g_sel)] : 0;
  ImGui::BeginDisabled(!has);
  const ImVec2 bw(S(150), 0);
  if (ImGui::Button(TR("Update Values"), bw)) {
    std::string e;
    if (!UpdateLink(s, cur, &e)) s.ShowMessage("YCGrid", e);
  }
  if (ImGui::Button(TR("Change Source..."), bw)) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = s.hwnd;
    ofn.lpstrFilter = L"Workbooks (*.ycg;*.xlsx;*.xlsm;*.csv)\0*.ycg;*.xlsx;*.xlsm;*.csv\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    std::wstring answer;
    const bool hidden = NoFileDialog(s.hwnd, TR("Change Source"), &answer);
    if (hidden ? !answer.empty() : ::GetOpenFileNameW(&ofn) != 0) {
      const std::string p = ToUtf8(hidden ? answer : std::wstring(file));
      s.engine->Call([&](Document& d) { d.ChangeLinkSource(cur, p); });
      s.engine->Sync();
      s.snap = s.engine->Current();
      std::string e;
      if (!UpdateLink(s, cur, &e)) s.ShowMessage("YCGrid", e);
    }
  }
  if (ImGui::Button(TR("Open Source"), bw)) {
    const std::wstring src = ResolveSource(s, links[cur].path);
    if (src.empty()) s.ShowError("YCGrid", TR("The source wasn't found: ") + links[cur].path);
    else OpenInNewWindow(src);
  }
  if (ImGui::Button(TR("Break Link"), bw)) {
    s.engine->PostUser(TR("Break Link"), [cur](Document& d) { d.BreakLink(cur); });
  }
  ImGui::EndDisabled();
  ImGui::EndGroup();
  ImGui::Separator();
  ImGui::TextDisabled("%s", TR("Values are read when the workbook opens and whenever a source is saved."));
  ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + S(560) + ImGui::GetStyle().ItemSpacing.x + S(150) - S(96));  // under the buttons
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (ImGui::Button(TR("Close"), ImVec2(S(96), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

}  // namespace yc::app
