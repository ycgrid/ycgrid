// SPDX-License-Identifier: Apache-2.0
// replay_ui.cpp - [YC] Trading > Record / Replay: the commands, the replay bar and the status bar chips (replay.cpp)
#include "app_state.h"
#include "settings.h"
#include "core/i18n.h"
#include "python_bridge.h"
#include "ui/commands/command_registry.h"
#include "ui/fonts/fonts.h"
#include "ui/icons/yc_icons.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>

namespace yc::app {

namespace {

float S(float v) { return v * ImGui::GetFontSize() / 16.0f; }

constexpr double kSpeeds[] = {0.25, 0.5, 1, 2, 5, 10, 50, 0};

std::string SpeedText(double sp) {
  if (sp <= 0) return TR("Fastest");
  char b[32];
  std::snprintf(b, sizeof(b), sp < 1 ? "%.2gx" : "%gx", sp);
  return b;
}

// recorded time: local date and time with milliseconds
std::string TapeClock(int64_t start_unix_ms, double pos_s) {
  const int64_t ms = start_unix_ms + static_cast<int64_t>(std::llround(pos_s * 1000.0));
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  if (localtime_s(&tm, &t) != 0) return {};
  char b[64];
  std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms % 1000));
  return b;
}

std::string Duration(double s) {
  const int64_t t = static_cast<int64_t>(s);
  char b[32];
  if (t >= 3600)
    std::snprintf(b, sizeof(b), "%lld:%02lld:%02lld", static_cast<long long>(t / 3600), static_cast<long long>(t / 60 % 60), static_cast<long long>(t % 60));
  else
    std::snprintf(b, sizeof(b), "%lld:%02lld", static_cast<long long>(t / 60), static_cast<long long>(t % 60));
  return b;
}

std::wstring RecordingsFolder(const AppState& s) {
  namespace fs = std::filesystem;
  fs::path dir;
  if (!s.file_path.empty()) {
    dir = fs::path(s.file_path).parent_path() / L"recordings";
  } else if (!UserStateSaved()) {
    dir = fs::temp_directory_path() / L"YCGrid-hidden-projects" / L"recordings";  // a hidden instance: not in Documents
  } else {
    PWSTR docs = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) dir = fs::path(docs) / L"YCGrid" / L"recordings";
    if (docs) ::CoTaskMemFree(docs);
  }
  return dir.wstring();
}

std::wstring PickTape(HWND owner, const std::wstring& folder) {
  if (std::wstring a; NoFileDialog(owner, TR("Replay"), &a)) return a;
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::wstring result;
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    const COMDLG_FILTERSPEC types[] = {{TRW(L"YCGrid Recording (*.yctape)"), L"*.yctape"}};
    dlg->SetFileTypes(1, types);
    IShellItem* start = nullptr;
    if (!folder.empty() && SUCCEEDED(::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&start))) && start) {
      dlg->SetFolder(start);
      start->Release();
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (SUCCEEDED(dlg->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item)) && item) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
          result = p;
          ::CoTaskMemFree(p);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(init)) ::CoUninitialize();
  return result;
}

struct ReplayUi {
  bool focus_bar = false;
  float drag_pos = -1;  // the slider while it is dragged (seconds)
  bool compare_open = false;
  io::MV compare;  // the last comparison (rows)
  double compared_at = -1;
};
ReplayUi& U() {
  static ReplayUi u;
  return u;
}

// stops the recording and says on the status bar what came of it
void StopRecordingNow(AppState& s) {
  const std::string path = s.python->GetTapeStatus().rec_path;
  const int64_t frames = s.python->StopRecording();
  if (frames == 0) s.SetStatus(TR("Nothing was recorded: no live data arrived"), 10.0);
  else if (frames > 0) s.SetStatus(TR("Recording saved: ") + path, 10.0);
}

}  // namespace

std::wstring DefaultRecordingPath(const AppState& s) {
  namespace fs = std::filesystem;
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  wchar_t name[64];
  std::swprintf(name, 64, L"%04d%02d%02d-%02d%02d%02d.yctape", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  const fs::path dir = RecordingsFolder(s);
  std::error_code ec;
  fs::create_directories(dir, ec);
  return (dir / name).wstring();
}

void RegisterReplayCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  ui::Command rec = make("RecordData", TR("Record"), ui::icon::Record,
                         TR("Record the live data on the screen to a .yctape file (in the workbook's recordings folder): the "
                            "market-data tables, or everything when no table is marked as market data. Click again, or the "
                            "REC chip on the status bar, to stop."));
  rec.toggle = true;
  R.Add(rec).execute = [&s](ui::Command&) {
    if (!s.python) return;
    if (s.python->GetTapeStatus().recording) {
      StopRecordingNow(s);
      return;
    }
    std::string err;
    // the figures too: with no table marked as market data, Record takes the whole screen
    const PythonBridge::PlotRecords figs = FigureSnapshot();
    if (!s.python->StartRecording(DefaultRecordingPath(s), {}, &err, &figs)) s.ShowError("YCGrid", err);
  };
  R.Add(make("ReplayData", TR("Replay"), ui::icon::History,
             TR("Replay a recording into the same bound tables: the screen and its Python code run on recorded data, unchanged.")))
      .execute = [&s](ui::Command&) {
    if (!s.python) return;
    const std::wstring p = PickTape(s.hwnd, RecordingsFolder(s));
    if (p.empty()) return;
    std::string err;
    if (!s.python->StartReplay(p, 1, 0, false, &err)) s.ShowError("YCGrid", err);
    U().focus_bar = true;
  };
  ui::Command all = make("RecordEverything", TR("Record Everything"), ui::icon::Record,
                         TR("The time machine: record every bound table (the app's own positions and orders too), the charts and the "
                            "alerts, to replay the whole screen later and step through what happened."));
  all.toggle = true;
  R.Add(all).execute = [&s](ui::Command&) {
    if (!s.python) return;
    if (s.python->GetTapeStatus().recording) {
      StopRecordingNow(s);
      return;
    }
    std::string err;
    const PythonBridge::PlotRecords figs = FigureSnapshot();  // the UI thread owns the figures
    if (!s.python->StartRecording(DefaultRecordingPath(s), {"*"}, &err, &figs)) s.ShowError("YCGrid", err);
  };
  R.Add(make("ReplayLive", TR("Back to\nLive"), ui::icon::Live, TR("Stop the replay: the bound tables show live data again.")))
      .execute = [&s](ui::Command&) {
    if (s.python) s.python->StopReplay();
  };
}

float DrawReplayStatus(AppState& s, ImDrawList* dl, const ImRect& bar, float left, float ty) {
  if (!s.python) return left;
  const PythonBridge::TapeStatus st = s.python->GetTapeStatus();
  if (ui::Command* c = ui::Commands().Find("RecordData")) c->pressed = st.recording;
  if (ui::Command* c = ui::Commands().Find("RecordEverything")) c->pressed = st.recording;
  if (ui::Command* c = ui::Commands().Find("ReplayLive")) c->enabled = st.mode != 0;
  auto chip = [&](const std::string& text, ImU32 bg, const std::string& tip, const char* test_label) -> bool {
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImRect r(ImVec2(left, bar.Min.y + S(3)), ImVec2(left + ts.x + S(14), bar.Max.y - S(3)));
    ui::NoteUiItem(test_label, r);  // tests click it as a person does (ui.items)
    dl->AddRectFilled(r.Min, r.Max, bg, S(3));
    dl->AddText(ImVec2(r.Min.x + S(7), ty), IM_COL32_WHITE, text.c_str());
    left = r.Max.x + S(8);
    const bool hov = ImGui::IsMouseHoveringRect(r.Min, r.Max);
    if (hov) ImGui::SetTooltip("%s", tip.c_str());
    return hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  };
  if (st.recording) {
    const bool blink = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
    const std::string t = std::string(blink ? "\xE2\x97\x8F " : "  ") + TR("REC") + " " + Duration(st.rec_seconds);
    if (chip(t, IM_COL32(0xC6, 0x28, 0x28, 255), TR("Recording to ") + st.rec_path + "\n" + std::to_string(st.rec_frames) + " " + TR("frames") + "\n" + TR("Click to stop"), "##rec_chip"))
      StopRecordingNow(s);
  }
  if (st.mode != 0) {
    const std::string t = std::string(TR("REPLAY")) + " " + SpeedText(st.speed) + (st.mode == 2 ? " " + std::string(TR("paused")) : "");
    if (chip(t, IM_COL32(0x6A, 0x1B, 0x9A, 255), TR("The bound tables show recorded data: ") + st.path, "##replay_chip")) U().focus_bar = true;
  }
  return left;
}

void DrawReplayFrame(AppState& s) {
  if (!s.python) return;
  const PythonBridge::TapeStatus st = s.python->GetTapeStatus();
  ReplayUi& u = U();
  if (st.mode == 0) {
    u.drag_pos = -1;
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - S(80)), ImGuiCond_FirstUseEver, ImVec2(0.5f, 1));
  ImGui::SetNextWindowSize(ImVec2(S(720), 0), ImGuiCond_FirstUseEver);
  if (u.focus_bar) {
    ImGui::SetNextWindowFocus();
    u.focus_bar = false;
  }
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(0x6A, 0x1B, 0x9A, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(0x7B, 0x1F, 0xA2, 255));
  const std::string title = std::string(TR("Replay")) + " - " + std::filesystem::path(FromUtf8(st.path)).filename().string() + "###yc_replay";
  bool open = true;
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
  const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor(3);
  if (visible) {
    auto icon_button = [](const char* glyph, const char* id) {
      ImGui::PushFont(ui::F().icons, ImGui::GetFontSize() * 1.15f);
      const bool hit = ImGui::Button((std::string(glyph) + id).c_str());
      ImGui::PopFont();
      return hit;
    };
    if (icon_button(ui::icon::Previous, "##start")) s.python->SeekReplay(0);
    ImGui::SetItemTooltip("%s", TR("Back to the start"));
    ImGui::SameLine();
    const bool playing = st.mode == 1;
    if (icon_button(playing ? ui::icon::Pause : ui::icon::Play, "##play")) s.python->PauseReplay(playing);
    ImGui::SetItemTooltip("%s", playing ? TR("Pause") : TR("Play"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(72));
    if (ImGui::BeginCombo("##speed", SpeedText(st.speed).c_str())) {
      for (double sp : kSpeeds) {
        if (ImGui::Selectable(SpeedText(sp).c_str(), sp == st.speed)) s.python->SetReplaySpeed(sp);
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", TR("Replay speed"));
    ImGui::SameLine();
    const float right = ImGui::CalcTextSize(TR("Loop")).x + ImGui::CalcTextSize(TR("Back to Live")).x + S(80);
    ImGui::SetNextItemWidth(std::max(S(120), ImGui::GetContentRegionAvail().x - right));
    float pos = u.drag_pos >= 0 ? u.drag_pos : static_cast<float>(st.pos_s);
    const std::string fmt = Duration(pos) + " / " + Duration(st.length_s);
    if (ImGui::SliderFloat("##pos", &pos, 0, static_cast<float>(std::max(st.length_s, 0.001)), fmt.c_str())) u.drag_pos = pos;
    {  // the events of the recording on the timeline: hover for what happened, click to go there
      const ImRect sr(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float grab = ImGui::GetStyle().GrabMinSize * 0.5f + ImGui::GetStyle().FramePadding.x;
      const float x0 = sr.Min.x + grab, x1 = sr.Max.x - grab;
      for (const auto& mk : st.marks) {
        if (st.length_s <= 0) break;
        const float x = x0 + (x1 - x0) * static_cast<float>(std::clamp(mk.t / st.length_s, 0.0, 1.0));
        const ImU32 c = mk.kind == "alert" ? IM_COL32(0xFF, 0x3B, 0x3B, 255) : mk.kind == "fill" ? IM_COL32(0xFF, 0xD4, 0x00, 255)
                        : mk.kind == "order" ? IM_COL32(0x00, 0xC8, 0xFF, 255) : IM_COL32(0xFF, 0xFF, 0xFF, 220);
        const ImVec2 a(x, sr.Max.y - 1), b(x - S(4), sr.Max.y + S(5)), d(x + S(4), sr.Max.y + S(5));
        dl->AddTriangleFilled(a, b, d, c);
        if (ImGui::IsMouseHoveringRect(ImVec2(x - S(5), sr.Max.y - 2), ImVec2(x + S(5), sr.Max.y + S(7)))) {
          ImGui::SetTooltip("%s  %s\n%s", Duration(mk.t).c_str(), mk.kind.c_str(), mk.text.c_str());
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) s.python->SeekReplay(mk.t);
        }
      }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && u.drag_pos >= 0) {
      s.python->SeekReplay(u.drag_pos);
      u.drag_pos = -1;
    } else if (!ImGui::IsItemActive()) {
      u.drag_pos = -1;
    }
    ImGui::SameLine();
    bool loop = st.loop;
    if (ImGui::Checkbox(TR("Loop"), &loop)) s.python->SetReplayLoop(loop);
    ImGui::SameLine();
    if (ImGui::Button(TR("Back to Live"))) s.python->StopReplay();
    ImGui::TextDisabled("%s", TapeClock(st.start_unix_ms, u.drag_pos >= 0 ? u.drag_pos : st.pos_s).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  %s %llu / %llu", TR("frame"), static_cast<unsigned long long>(st.frame), static_cast<unsigned long long>(st.frames));
    if (st.mode == 3) {
      ImGui::SameLine();
      ImGui::TextDisabled("  %s", TR("(end of the recording)"));
    }
    if (st.dropped) {
      ImGui::SameLine();
      ImGui::TextDisabled("  %s %llu", TR("live frames ignored:"), static_cast<unsigned long long>(st.dropped));
    }
    // compare two moments: pin one, move to another, see what changed
    if (ImGui::SmallButton(TR("Pin This Moment"))) {
      s.python->PinTables();
      u.compare_open = false;
    }
    ImGui::SetItemTooltip("%s", TR("Remember the tables as they are now; move to another moment and press Compare."));
    if (st.pinned) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s %s", TR("pinned at"), Duration(st.pin_s).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton(TR("Compare"))) {
        u.compare = s.python->CompareWithPin(2000);
        u.compared_at = st.pos_s;
        u.compare_open = true;
      }
    }
  }
  ImGui::End();
  if (u.compare_open) {
    ImGui::SetNextWindowSize(ImVec2(S(620), S(360)), ImGuiCond_FirstUseEver);
    PlaceNewWindow(WindowPlace::Center);
    const std::string ct = std::string(TR("Compare")) + "  " + Duration(st.pin_s) + "  ->  " + Duration(u.compared_at) + "###yc_replay_compare";
    if (ImGui::Begin(ct.c_str(), &u.compare_open, ImGuiWindowFlags_NoSavedSettings)) {
      if (u.compare.a.empty()) ImGui::TextDisabled("%s", TR("Nothing changed between the two moments."));
      else if (ImGui::BeginTable("##cmp", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {TR("Table"), TR("Key"), TR("Column"), TR("Then"), TR("Now"), TR("Change")}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(u.compare.a.size()));
        while (clip.Step()) {
          for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const io::MV& r = u.compare.a[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            for (const char* k : {"table", "key", "column", "then", "now"}) {
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(r.GetStr(k).c_str());
            }
            ImGui::TableNextColumn();
            if (const io::MV* ch = r.Get("change")) {
              const double v = ch->Num();
              ImGui::TextColored(v > 0 ? ImVec4(0.9f, 0.22f, 0.23f, 1) : v < 0 ? ImVec4(0.09f, 0.64f, 0.29f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%+.6g", v);
            }
          }
        }
        ImGui::EndTable();
      }
    }
    ImGui::End();
  }
  if (!open) s.python->StopReplay();
}

}  // namespace yc::app
