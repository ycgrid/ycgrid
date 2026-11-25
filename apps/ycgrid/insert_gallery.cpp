// SPDX-License-Identifier: Apache-2.0
// insert_gallery.cpp - Insert > Icons (a gallery of pictures drawn by YCGrid) and Insert > WordArt (styled text boxes)
//
// Icons: Excel's Insert Icons dialog fetches pictures from Microsoft's service; YCGrid draws its own set from
// polygons (the preset shapes' outlines plus a few drawn here) and inserts each as a picture (a PNG made at insert
// time), so they print, copy and save like any picture.
#include "app_state.h"
#include "core/i18n.h"
#include "engine/shape_geometry.h"
#include "screenshot.h"

#include "commands/command_registry.h"
#include "fonts/cell_fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace yc::app {

using engine::Document;
using engine::ShapePt;
using ui::S;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPx = 4.0 / 3.0;

// ---- icons ----
struct IconPoly {
  std::vector<ShapePt> pts;  // in a 100 x 100 box
  bool hole = false;         // cut out of what was drawn before it
};
using IconDraw = std::vector<IconPoly>;

std::vector<ShapePt> Circle(double cx, double cy, double r) {
  std::vector<ShapePt> v;
  for (int i = 0; i < 48; ++i) v.push_back({cx + r * std::cos(2 * kPi * i / 48), cy + r * std::sin(2 * kPi * i / 48)});
  return v;
}
std::vector<ShapePt> Box(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }
// a thick segment from a to b
std::vector<ShapePt> Bar(ShapePt a, ShapePt b, double w) {
  const double dx = b.x - a.x, dy = b.y - a.y, l = std::max(1e-9, std::hypot(dx, dy)), nx = -dy / l * w / 2, ny = dx / l * w / 2;
  return {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
}
// the filled pieces of a preset shape in the box (x0, y0)-(x1, y1)
void Preset(IconDraw& d, int type, double x0, double y0, double x1, double y1, double turn = 0) {
  engine::ShapeGeom g;
  if (!engine::AutoShapeGeometry(type, x1 - x0, y1 - y0, &g)) return;
  const double cx = (x1 - x0) / 2, cy = (y1 - y0) / 2, a = turn * kPi / 180, ca = std::cos(a), sa = std::sin(a);
  for (const auto& f : g.fills) {
    IconPoly p;
    for (const auto& q : f.pts) p.pts.push_back({x0 + cx + (q.x - cx) * ca - (q.y - cy) * sa, y0 + cy + (q.x - cx) * sa + (q.y - cy) * ca});
    d.push_back(std::move(p));
  }
}
void Add(IconDraw& d, std::vector<ShapePt> v, bool hole = false) { d.push_back({std::move(v), hole}); }

struct IconDef {
  const char* name;
  const char* category;
  std::function<void(IconDraw&)> draw;
};

const std::vector<IconDef>& Icons() {
  static const std::vector<IconDef> kIcons = {
      {TR("Arrow right"), TR("Arrows"), [](IconDraw& d) { Preset(d, 33, 8, 22, 92, 78); }},
      {TR("Arrow left"), TR("Arrows"), [](IconDraw& d) { Preset(d, 34, 8, 22, 92, 78); }},
      {TR("Arrow up"), TR("Arrows"), [](IconDraw& d) { Preset(d, 35, 22, 8, 78, 92); }},
      {TR("Arrow down"), TR("Arrows"), [](IconDraw& d) { Preset(d, 36, 22, 8, 78, 92); }},
      {TR("Refresh"), TR("Arrows"), [](IconDraw& d) { Preset(d, 60, 8, 8, 92, 92); }},
      {TR("Trend up"), TR("Arrows"),
       [](IconDraw& d) {
         Add(d, Bar({10, 82}, {38, 52}, 9));
         Add(d, Bar({38, 52}, {55, 66}, 9));
         Add(d, Bar({55, 66}, {78, 38}, 9));
         Add(d, {{70, 26}, {92, 18}, {86, 42}});
       }},
      {TR("Bar chart"), TR("Analytics"),
       [](IconDraw& d) {
         Add(d, Box(14, 56, 30, 88));
         Add(d, Box(42, 34, 58, 88));
         Add(d, Box(70, 14, 86, 88));
         Add(d, Box(6, 88, 94, 94));
       }},
      {TR("Line chart"), TR("Analytics"),
       [](IconDraw& d) {
         Add(d, Box(8, 8, 14, 92));
         Add(d, Box(8, 86, 92, 92));
         Add(d, Bar({20, 72}, {42, 46}, 7));
         Add(d, Bar({42, 46}, {60, 58}, 7));
         Add(d, Bar({60, 58}, {88, 22}, 7));
       }},
      {TR("Pie chart"), TR("Analytics"),
       [](IconDraw& d) {
         Preset(d, 142, 6, 14, 86, 94);
         std::vector<ShapePt> w = {{52, 48}};
         for (int i = 0; i <= 16; ++i) w.push_back({52 + 40 * std::cos((270 + 90.0 * i / 16) * kPi / 180), 48 + 40 * std::sin((270 + 90.0 * i / 16) * kPi / 180)});
         Add(d, w);
       }},
      {TR("Target"), TR("Analytics"),
       [](IconDraw& d) {
         Add(d, Circle(50, 50, 45));
         Add(d, Circle(50, 50, 35), true);
         Add(d, Circle(50, 50, 25));
         Add(d, Circle(50, 50, 15), true);
         Add(d, Circle(50, 50, 7));
       }},
      {TR("Briefcase"), TR("Business"),
       [](IconDraw& d) {
         Preset(d, 5, 6, 28, 94, 88);
         Add(d, Box(34, 14, 66, 30));
         Add(d, Box(41, 20, 59, 30), true);
         Add(d, Box(6, 52, 94, 57), true);
       }},
      {TR("Clock"), TR("Business"),
       [](IconDraw& d) {
         Add(d, Circle(50, 50, 45));
         Add(d, Circle(50, 50, 38), true);
         Add(d, Bar({50, 52}, {50, 20}, 7));
         Add(d, Bar({48, 50}, {72, 62}, 7));
       }},
      {TR("Calendar"), TR("Business"),
       [](IconDraw& d) {
         Add(d, Box(8, 16, 92, 92));
         Add(d, Box(15, 36, 85, 85), true);
         Add(d, Box(24, 6, 32, 24));
         Add(d, Box(68, 6, 76, 24));
         for (int r = 0; r < 3; ++r) {
           for (int c = 0; c < 4; ++c) Add(d, Box(20 + c * 16, 42 + r * 14, 30 + c * 16, 51 + r * 14));
         }
       }},
      {TR("Document"), TR("Business"),
       [](IconDraw& d) {
         Preset(d, 16, 16, 6, 84, 94, 180);
         for (int k = 0; k < 4; ++k) Add(d, Box(28, 40 + k * 12, 72, 45 + k * 12), true);
       }},
      {TR("Envelope"), TR("Communication"),
       [](IconDraw& d) {
         Add(d, Box(6, 20, 94, 80));
         Add(d, {{6, 20}, {50, 54}, {94, 20}, {94, 28}, {50, 62}, {6, 28}}, true);
       }},
      {TR("Speech"), TR("Communication"), [](IconDraw& d) { Preset(d, 106, 8, 10, 92, 74); }},
      {TR("Chat"), TR("Communication"),
       [](IconDraw& d) {
         Preset(d, 106, 4, 6, 64, 50);
         Preset(d, 106, 36, 38, 96, 82, 0);
       }},
      {TR("Bell"), TR("Communication"),
       [](IconDraw& d) {
         std::vector<ShapePt> v = {{18, 76}};
         for (int i = 0; i <= 24; ++i) v.push_back({50 + 32 * std::cos((180 + 180.0 * i / 24) * kPi / 180), 46 + 34 * std::sin((180 + 180.0 * i / 24) * kPi / 180)});
         v.insert(v.end(), {{82, 76}, {90, 82}, {10, 82}});
         Add(d, v);
         Add(d, Circle(50, 88, 7));
         Add(d, Circle(50, 10, 5));
       }},
      {TR("Checkmark"), TR("Interface"), [](IconDraw& d) { Add(d, {{8, 52}, {21, 39}, {40, 58}, {79, 17}, {92, 30}, {40, 84}}); }},
      {TR("Close"), TR("Interface"), [](IconDraw& d) { Preset(d, 165, 0, 0, 100, 100); }},
      {TR("Add"), TR("Interface"), [](IconDraw& d) { Preset(d, 163, 0, 0, 100, 100); }},
      {TR("Search"), TR("Interface"),
       [](IconDraw& d) {
         Add(d, Circle(40, 40, 30));
         Add(d, Circle(40, 40, 21), true);
         Add(d, Bar({60, 60}, {90, 90}, 13));
       }},
      {TR("Settings"), TR("Interface"),
       [](IconDraw& d) {
         Preset(d, 150, 6, 6, 94, 94);
         Add(d, Circle(50, 50, 30));
         Add(d, Circle(50, 50, 14), true);
       }},
      {TR("Home"), TR("Interface"),
       [](IconDraw& d) {
         Add(d, {{4, 50}, {50, 8}, {96, 50}, {88, 56}, {50, 22}, {12, 56}});
         Add(d, {{20, 50}, {50, 24}, {80, 50}, {80, 92}, {20, 92}});
         Add(d, Box(42, 64, 58, 92), true);
       }},
      {TR("Lock"), TR("Interface"),
       [](IconDraw& d) {
         Preset(d, 20, 26, 10, 74, 62);
         Add(d, Box(26, 34, 36, 48));
         Add(d, Box(64, 34, 74, 48));
         Preset(d, 5, 16, 44, 84, 92);
         Add(d, Circle(50, 62, 7), true);
         Add(d, Box(47, 62, 53, 78), true);
       }},
      {TR("User"), TR("Interface"),
       [](IconDraw& d) {
         Add(d, Circle(50, 28, 20));
         std::vector<ShapePt> v;
         for (int i = 0; i <= 24; ++i) v.push_back({50 + 38 * std::cos((180 + 180.0 * i / 24) * kPi / 180), 96 + 42 * std::sin((180 + 180.0 * i / 24) * kPi / 180)});
         Add(d, v);
       }},
      {TR("Star"), TR("Interface"), [](IconDraw& d) { Preset(d, 92, 4, 4, 96, 94); }},
      {TR("Heart"), TR("Interface"), [](IconDraw& d) { Preset(d, 21, 6, 10, 94, 90); }},
      {TR("Flag"), TR("Interface"),
       [](IconDraw& d) {
         Add(d, Box(14, 6, 21, 96));
         Add(d, {{21, 10}, {88, 10}, {72, 32}, {88, 54}, {21, 54}});
       }},
      {TR("Warning"), TR("Signs and symbols"),
       [](IconDraw& d) {
         Add(d, {{50, 6}, {96, 90}, {4, 90}});
         Add(d, Box(45, 34, 55, 64), true);
         Add(d, Circle(50, 76, 5.5), true);
       }},
      {TR("Information"), TR("Signs and symbols"),
       [](IconDraw& d) {
         Add(d, Circle(50, 50, 45));
         Add(d, Box(44, 42, 56, 78), true);
         Add(d, Circle(50, 28, 7), true);
       }},
      {TR("Lightbulb"), TR("Signs and symbols"),
       [](IconDraw& d) {
         Add(d, Circle(50, 38, 30));
         Add(d, Box(36, 58, 64, 72));
         Add(d, Box(38, 76, 62, 82));
         Add(d, Box(42, 86, 58, 92));
       }},
      {TR("Lightning"), TR("Signs and symbols"), [](IconDraw& d) { Preset(d, 22, 14, 4, 86, 96); }},
      {TR("Sun"), TR("Weather and seasons"), [](IconDraw& d) { Preset(d, 23, 2, 2, 98, 98); }},
      {TR("Cloud"), TR("Weather and seasons"), [](IconDraw& d) { Preset(d, 179, 4, 20, 96, 82); }},
      {TR("Moon"), TR("Weather and seasons"), [](IconDraw& d) { Preset(d, 24, 22, 6, 78, 94); }},
      {TR("Raindrop"), TR("Weather and seasons"), [](IconDraw& d) { Preset(d, 160, 18, 18, 82, 82, -45); }},
      {TR("Snowflake"), TR("Weather and seasons"),
       [](IconDraw& d) {
         for (int k = 0; k < 3; ++k) {
           const double a = k * kPi / 3;
           Add(d, Bar({50 - 44 * std::cos(a), 50 - 44 * std::sin(a)}, {50 + 44 * std::cos(a), 50 + 44 * std::sin(a)}, 8));
         }
       }},
  };
  return kIcons;
}

// the icon as RGBA pixels (n x n, dark gray on transparent), anti-aliased by 4 x 4 samples per pixel
Image RenderIcon(const IconDef& def, int n, uint32_t rgb = 0x404040) {
  IconDraw d;
  def.draw(d);
  const int k = 4, N = n * k;
  std::vector<uint8_t> on(static_cast<size_t>(N) * N, 0);
  const double sc = N / 100.0;
  for (const IconPoly& p : d) {
    const size_t m = p.pts.size();
    if (m < 3) continue;
    std::vector<double> xs;
    for (int y = 0; y < N; ++y) {
      const double py = (y + 0.5) / sc;
      xs.clear();
      for (size_t i = 0, j = m - 1; i < m; j = i++) {
        const ShapePt& a = p.pts[i];
        const ShapePt& b = p.pts[j];
        if ((a.y > py) != (b.y > py)) xs.push_back((b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x);
      }
      std::sort(xs.begin(), xs.end());
      for (size_t q = 0; q + 1 < xs.size(); q += 2) {
        const int x0 = std::max(0, static_cast<int>(std::ceil(xs[q] * sc - 0.5))), x1 = std::min(N - 1, static_cast<int>(std::floor(xs[q + 1] * sc - 0.5)));
        for (int x = x0; x <= x1; ++x) on[static_cast<size_t>(y) * N + x] = p.hole ? 0 : 1;
      }
    }
  }
  Image img;
  img.w = img.h = n;
  img.rgba.assign(static_cast<size_t>(n) * n * 4, 0);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      int c = 0;
      for (int sy = 0; sy < k; ++sy) {
        for (int sx = 0; sx < k; ++sx) c += on[static_cast<size_t>(y * k + sy) * N + x * k + sx];
      }
      uint8_t* px = &img.rgba[(static_cast<size_t>(y) * n + x) * 4];
      px[0] = (rgb >> 16) & 0xFF, px[1] = (rgb >> 8) & 0xFF, px[2] = rgb & 0xFF;
      px[3] = static_cast<uint8_t>(c * 255 / (k * k));
    }
  }
  return img;
}

struct GalleryUi {
  bool icons_open = false, icons_request = false;
  std::set<int> picked;
  int category = -1;  // -1 all
  char search[64] = "";
  std::map<int, ImTextureID> thumbs;
};
GalleryUi& G() {
  static GalleryUi g;
  return g;
}

ImTextureID IconThumb(int i) {
  auto& t = G().thumbs[i];
  if (!t) {
    std::string png;
    if (EncodePng(RenderIcon(Icons()[static_cast<size_t>(i)], 64), &png)) t = TextureFromImage(png);
  }
  return t;
}

void ActivePoint(AppState& s, double* left, double* top) {
  const model::Sheet* sh = s.FocusSheet();
  const auto& sel = s.FocusView().sel;
  *left = sh ? static_cast<double>(sh->cols->Offset(sel.act_c)) / kPx : 0;
  *top = sh ? static_cast<double>(sh->rows->Offset(sel.act_r)) / kPx : 0;
}

// the picked icons as pictures at the active cell (each a step right of the one before), all of them selected
void InsertIcons(AppState& s, const std::vector<int>& which) {
  double l = 0, t = 0;
  ActivePoint(s, &l, &t);
  std::vector<model::ShapeObj> shapes;
  for (size_t k = 0; k < which.size(); ++k) {
    model::ShapeObj sh;
    sh.kind = model::ShapeKind::Picture;
    if (!EncodePng(RenderIcon(Icons()[static_cast<size_t>(which[k])], 192), &sh.image)) continue;
    sh.name = std::string("Graphic");
    sh.left = l + static_cast<double>(k) * 80;
    sh.top = t;
    sh.width = sh.height = 72;  // one inch, as Excel inserts an icon
    sh.alt_text = Icons()[static_cast<size_t>(which[k])].name;
    sh.line_visible = false;
    shapes.push_back(std::move(sh));
  }
  if (shapes.empty()) return;
  const uint32_t sheet = s.focus_sheet;
  const std::vector<uint32_t> ids = s.engine->Call([&](Document& d) {
    std::vector<uint32_t> v;
    d.Begin(TR("Insert Icons"));
    for (auto x : shapes) {
      x.name.clear();
      v.push_back(d.AddShape(sheet, x));
    }
    d.Commit();
    return v;
  });
  if (!ids.empty() && ids.front()) SelectInsertedShape(s, sheet, ids.front());
}

void DrawIconsDialog(AppState& s) {
  GalleryUi& g = G();
  const char* title = TR("Insert Icons###yc_icons");
  if (g.icons_request) {
    g.icons_request = false;
    ImGui::OpenPopup(title);
  }
  ImGui::SetNextWindowSize(ImVec2(S(640), S(520)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) return;
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##search", TR("Search"), g.search, sizeof(g.search));
  // the categories as Excel's chips
  std::vector<const char*> cats;
  for (const auto& d : Icons()) {
    if (std::find_if(cats.begin(), cats.end(), [&](const char* c) { return std::strcmp(c, d.category) == 0; }) == cats.end()) cats.push_back(d.category);
  }
  if (ImGui::Selectable(TR("All##icons"), g.category < 0, 0, ImVec2(ImGui::CalcTextSize(TR("All##icons")).x, 0))) g.category = -1;
  for (int c = 0; c < static_cast<int>(cats.size()); ++c) {
    ImGui::SameLine(0, S(14));
    ImGui::PushID(c);
    if (ImGui::Selectable(cats[static_cast<size_t>(c)], g.category == c, 0, ImVec2(ImGui::CalcTextSize(cats[static_cast<size_t>(c)]).x, 0))) g.category = c;
    ImGui::PopID();
  }
  ImGui::Separator();
  ImGui::BeginChild("##grid", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() - S(8)));
  const float tile = S(72);
  const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (tile + S(8))));
  std::string q = g.search;
  std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  int col = 0;
  for (int i = 0; i < static_cast<int>(Icons().size()); ++i) {
    const IconDef& d = Icons()[static_cast<size_t>(i)];
    if (g.category >= 0 && std::strcmp(d.category, cats[static_cast<size_t>(g.category)]) != 0) continue;
    std::string nm = d.name;
    std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (!q.empty() && nm.find(q) == std::string::npos) continue;
    if (col) ImGui::SameLine(0, S(8));
    ImGui::PushID(i);
    const bool on = g.picked.count(i) > 0;
    if (ImGui::Selectable("##icon", on, 0, ImVec2(tile, tile))) {
      if (on) g.picked.erase(i);
      else g.picked.insert(i);
    }
    ui::NoteUiItem((std::string("icon:") + d.name).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    const ImVec2 a = ImGui::GetItemRectMin();
    if (ImTextureID t = IconThumb(i)) ImGui::GetWindowDrawList()->AddImage(ImTextureRef(t), a + ImVec2(S(8), S(8)), a + ImVec2(tile - S(8), tile - S(8)));
    if (on) ImGui::GetWindowDrawList()->AddRect(a, a + ImVec2(tile, tile), IM_COL32(0x21, 0x73, 0x46, 255), 0.0f, S(2));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.name);
    ImGui::PopID();
    col = (col + 1) % per_row;
  }
  ImGui::EndChild();
  const std::string ins = std::string(TR("Insert")) + (g.picked.empty() ? std::string() : " (" + std::to_string(g.picked.size()) + ")");
  ImGui::BeginDisabled(g.picked.empty());
  if (ImGui::Button((ins + "###insert").c_str(), ImVec2(S(110), 0))) {
    InsertIcons(s, std::vector<int>(g.picked.begin(), g.picked.end()));
    close = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  if (close) {
    g.picked.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// ---- WordArt: Excel's 15 text styles (fill, outline, shadow of the letters) ----
struct WordArtStyle {
  const char* name;
  uint32_t fill, outline;  // ARGB (0: none)
  bool shadow, bold;
};
const WordArtStyle* WordArtStyles() {
  static const WordArtStyle k[] = {
      {TR("Fill: Black, Text color 1; Shadow"), 0xFF000000, 0, true, false},
      {TR("Fill: Blue, Accent color 1; Shadow"), 0xFF4472C4, 0, true, false},
      {TR("Fill: Orange, Accent color 2; Outline: Orange, Accent color 2"), 0xFFED7D31, 0xFFC55A11, false, false},
      {TR("Fill: White; Outline: Blue, Accent color 1; Glow: Blue, Accent color 1"), 0xFFFFFFFF, 0xFF4472C4, false, false},
      {TR("Fill: Gold, Accent color 4; Soft Bevel"), 0xFFFFC000, 0, false, true},
      {TR("Gradient Fill: Gray"), 0xFF7F7F7F, 0, false, true},
      {TR("Gradient Fill: Blue, Accent color 5; Reflection"), 0xFF5B9BD5, 0, true, true},
      {TR("Gradient Fill: Gold, Accent color 4; Outline: Gold, Accent color 4"), 0xFFFFD966, 0xFFBF9000, false, true},
      {TR("Fill: White; Outline: Blue, Accent color 1; Shadow"), 0xFFFFFFFF, 0xFF4472C4, true, false},
      {TR("Fill: Black, Text color 1; Outline: White, Background color 1; Shadow"), 0xFF000000, 0xFFFFFFFF, true, true},
      {TR("Fill: Blue, Accent color 1; Outline: White, Background color 1; Hard Shadow: Blue, Accent color 1"), 0xFF4472C4, 0xFFFFFFFF, true, true},
      {TR("Fill: White; Outline: Orange, Accent color 2; Hard Shadow: Orange, Accent color 2"), 0xFFFFFFFF, 0xFFED7D31, true, true},
      {TR("Fill: Gray, Accent color 3; Sharp Bevel"), 0xFFA5A5A5, 0, false, true},
      {TR("Fill: White; Outline: Blue, Accent color 5; Shadow"), 0xFFFFFFFF, 0xFF5B9BD5, true, false},
      {TR("Pattern Fill: White; Dark Upward Diagonal Stripe; Shadow"), 0xFFD9D9D9, 0xFF404040, true, true},
  };
  return k;
}
constexpr int kWordArtCount = 15;

ImU32 Col(uint32_t argb) { return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF); }

}  // namespace

// a WordArt text box with style k (0..14) at the active cell, its placeholder text waiting to be typed over
uint32_t InsertWordArt(AppState& s, int k, const std::string& text) {
  const WordArtStyle& st = WordArtStyles()[std::clamp(k, 0, kWordArtCount - 1)];
  model::ShapeObj sh;
  sh.kind = model::ShapeKind::TextBox;
  sh.fill_visible = sh.line_visible = false;
  sh.text = text.empty() ? std::string(TR("Your text here")) : text;
  sh.font_size = 54;
  sh.font_color = st.fill;
  sh.text_outline = st.outline;
  sh.text_shadow = st.shadow;
  sh.bold = st.bold;
  sh.halign = 1;
  sh.wrap = false;
  sh.autofit = true;
  ImFont* f = ui::CellFont("", sh.bold, false);
  const float w = f->CalcTextSizeA(static_cast<float>(sh.font_size), FLT_MAX, 0.0f, sh.text.c_str()).x;  // at 1 px per point
  sh.width = w + sh.margin_l + sh.margin_r + 4;
  sh.height = sh.font_size * 1.25 + sh.margin_t + sh.margin_b;
  ActivePoint(s, &sh.left, &sh.top);
  const uint32_t sheet = s.focus_sheet;
  const uint32_t id = s.engine->Call([&](Document& d) {
    d.Begin(TR("Insert WordArt"));
    const uint32_t v = d.AddShape(sheet, sh);
    d.Commit();
    return v;
  });
  if (id) {
    SelectInsertedShape(s, sheet, id);
    if (text.empty()) MarkPlaceholderText(id);
  }
  return id;
}

void RegisterInsertGallery(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("InsertIcons", TR("Icons"), ui::icon::Icons, TR("Insert an icon to communicate with symbols."))).execute = [](ui::Command&) {
    G().icons_request = true;
    G().picked.clear();
  };
  R.Add(make("InsertWordArtMenu", TR("WordArt"), ui::icon::WordArt, TR("Make your text stand out with WordArt."))).execute = [](ui::Command&) {};
  for (int k = 0; k < kWordArtCount; ++k) {
    const std::string id = "WordArt" + std::to_string(k);
    R.Add(make(id.c_str(), WordArtStyles()[k].name, nullptr, "")).execute = [&s, k](ui::Command&) { InsertWordArt(s, k, ""); };
  }
  // the WordArt gallery's tiles: an "A" in each style
  auto& st = ui::ribbon::St();
  st.menu_picture = [prev = st.menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
    if (cmd.rfind("WordArt", 0) == 0 && cmd.size() > 7 && std::isdigit(static_cast<unsigned char>(cmd[7]))) {
      if (dl) {
        const WordArtStyle& w = WordArtStyles()[std::clamp(std::atoi(cmd.c_str() + 7), 0, kWordArtCount - 1)];
        ImFont* f = ui::CellFont("", w.bold, false);
        const float px = h * 0.95f;
        const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, "A");
        const ImVec2 p(at.x + (h - sz.x) * 0.5f, at.y + (h - sz.y) * 0.5f);
        if (w.shadow) dl->AddText(f, px, p + ImVec2(1.5f, 1.5f), IM_COL32(0, 0, 0, 90), "A");
        if (w.outline) {
          for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1)}) dl->AddText(f, px, p + d, Col(w.outline), "A");
        }
        dl->AddText(f, px, p, Col(w.fill), "A");
      }
      return h;
    }
    return prev ? prev(cmd, dl, at, h) : 0.0f;
  };
}

// the WordArt drop-down: its 15 styles as tiles (the ribbon may be rebuilt when it is customized)
void UpdateWordArtMenu() {
  static std::vector<std::string> menu;
  if (menu.empty()) {
    menu.push_back("=");
    for (int k = 0; k < kWordArtCount; ++k) menu.push_back("WordArt" + std::to_string(k));
  }
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "InsertWordArtMenu" && it.menu != menu) it.menu = menu;
        }
      }
    }
  }
}

void DrawInsertGallery(AppState& s) { DrawIconsDialog(s); }

// for tests and Python: the icons' names (Insert Icons dialog order)
std::vector<std::string> IconNames() {
  std::vector<std::string> v;
  for (const auto& d : Icons()) v.push_back(d.name);
  return v;
}

}  // namespace yc::app
