// SPDX-License-Identifier: Apache-2.0
// shape_effects.cpp - Format Shape > Effects drawn on screen (shadow, reflection, glow, soft edges, bevel, 3-D
// rotation), the Shape Format tab's Shape Effects and Shape Styles menus, and Page Layout > Themes > Effects
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "model/shape_effects.h"
#include "model/theme.h"
#include "ribbon/ribbon.h"
#include "yc_ui_items.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace yc::app {

using engine::Document;
using model::ShapeEffects;
using model::ShapeObj;
using ui::S;

namespace {

constexpr float kPtPx = 4.0f / 3.0f;  // points -> sheet pixels (100%)

ImU32 WithAlpha(uint32_t argb, float alpha) {
  const int a = std::clamp(static_cast<int>(alpha * 255.0f + 0.5f), 0, 255);
  return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, a);
}
uint32_t Argb(ImU32 c) {  // ImGui color -> ARGB (with its alpha, at least 1 so 0 doesn't read as opaque)
  const uint32_t a = std::max<uint32_t>(1, (c >> IM_COL32_A_SHIFT) & 0xFF);
  return (a << 24) | (((c >> IM_COL32_R_SHIFT) & 0xFF) << 16) | (((c >> IM_COL32_G_SHIFT) & 0xFF) << 8) | ((c >> IM_COL32_B_SHIFT) & 0xFF);
}

// the shape as a flat silhouette of one color (shadows, glows): no text, a picture becomes its rectangle
ShapeObj Silhouette(const ShapeObj& sh, ImU32 col) {
  ShapeObj c = sh;
  c.text.clear();
  if (c.kind == model::ShapeKind::Picture) {
    c.kind = model::ShapeKind::Rectangle;
    c.image.clear();
  }
  c.fill_visible = true;
  c.fill_transparency = 0;
  c.fill_color = Argb(col);
  c.line_visible = sh.line_visible || sh.kind == model::ShapeKind::Line || sh.kind == model::ShapeKind::Arrow;
  c.line_color = Argb(col);
  c.fx = ShapeEffects{};
  return c;
}

void ScaleVerts(ImDrawList* dl, int v0, int v1, std::function<void(ImDrawVert&)> fn) {
  for (int i = v0; i < v1; ++i) fn(dl->VtxBuffer[i]);
}

ImU32 Brighten(ImU32 c, float f) {  // f > 1 lighter (towards white), < 1 darker
  auto ch = [&](int sh) {
    const float v = static_cast<float>((c >> sh) & 0xFF);
    const float o = f >= 1 ? v + (255 - v) * (f - 1) : v * f;
    return static_cast<ImU32>(std::clamp(o, 0.0f, 255.0f)) << sh;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

}  // namespace

void DrawShapeFx(ImDrawList* dl, const ShapeObj& sh, const ImRect& r, float scale) {
  const ShapeEffects fx = model::EffectiveEffects(sh.fx);
  const bool any = fx.shadow || fx.refl_size > 0 || fx.glow_size > 0 || fx.soft_edge > 0 || fx.bevel_top || fx.bevel_bottom || fx.rot_x != 0 ||
                   fx.rot_y != 0 || fx.rot_z != 0 || fx.perspective != 0;
  if (!any) {
    DrawShapeOnScreen(dl, sh, r, scale);
    return;
  }
  const float k = kPtPx * scale;  // screen pixels per point
  const int v_all = dl->VtxBuffer.Size;
  const ImVec2 c = r.GetCenter();
  // ---- glow: the silhouette grown in a few layers of the glow color ----
  if (fx.glow_size > 0) {
    const float g = fx.glow_size * k;
    for (int layer = 6; layer >= 1; --layer) {  // a soft halo: the outer layers fainter
      const float e = g * static_cast<float>(layer) / 6.0f;
      DrawShapeOnScreen(dl, Silhouette(sh, WithAlpha(fx.glow_color, (1 - fx.glow_transparency) * 0.22f)), ImRect(r.Min - ImVec2(e, e), r.Max + ImVec2(e, e)), scale);
    }
  }
  // ---- outer / perspective shadow: the silhouette offset (a few copies spread by the blur) ----
  if (fx.shadow == 1 || fx.shadow == 3) {
    const float a = fx.shadow_angle * 3.14159265f / 180.0f;
    const ImVec2 off(std::cos(a) * fx.shadow_distance * k, std::sin(a) * fx.shadow_distance * k);
    const float sz = std::max(0.1f, fx.shadow_size / 100.0f);
    ImRect sr(c + (r.Min - c) * sz + off, c + (r.Max - c) * sz + off);
    // the blur: copies on two rings around the offset, each faint (their overlap is dark in the middle, light at the edge)
    const int copies = fx.shadow_blur > 0.5f ? 13 : 1;
    const float blur = fx.shadow_blur * k * 0.5f;
    const float alpha = (1 - fx.shadow_transparency) * (copies > 1 ? 0.14f : 1.0f);
    for (int i = 0; i < copies; ++i) {
      const float t = 6.2831853f * static_cast<float>(i) / 6.0f;
      const float ring = i == 0 ? 0.0f : i <= 6 ? 0.45f : 0.9f;
      const ImVec2 j = copies > 1 ? ImVec2(std::cos(t + (i > 6 ? 0.5f : 0.0f)) * blur * ring, std::sin(t + (i > 6 ? 0.5f : 0.0f)) * blur * ring) : ImVec2(0, 0);
      const int v0 = dl->VtxBuffer.Size;
      DrawShapeOnScreen(dl, Silhouette(sh, WithAlpha(fx.shadow_color, alpha)), ImRect(sr.Min + j, sr.Max + j), scale);
      if (fx.shadow == 3) {
        // perspective: the shadow lies on the ground behind the shape (flattened to a third, leaning back)
        const float base = sr.Max.y;
        ScaleVerts(dl, v0, dl->VtxBuffer.Size, [&](ImDrawVert& v) {
          const float dy = base - v.pos.y;
          v.pos.y = base - dy * 0.33f;
          v.pos.x += dy * 0.5f;
        });
      }
    }
  }
  // ---- the shape itself (soft edges: nested copies fading out towards the outline, then its text) ----
  const int v_shape = dl->VtxBuffer.Size;
  if (fx.soft_edge > 0) {
    const float e = fx.soft_edge * k;
    ShapeObj body = sh;
    body.text.clear();
    body.line_visible = false;
    body.fx = ShapeEffects{};
    for (int layer = 0; layer < 7; ++layer) {
      const float inset = e * static_cast<float>(layer) / 7.0f;
      if (r.GetWidth() - 2 * inset < 1 || r.GetHeight() - 2 * inset < 1) break;
      body.fill_transparency = 1.0f - (1.0f - sh.fill_transparency) * (layer == 6 ? 0.6f : 0.18f);
      DrawShapeOnScreen(dl, body, ImRect(r.Min + ImVec2(inset, inset), r.Max - ImVec2(inset, inset)), scale);
    }
    ShapeObj text = sh;
    text.fill_visible = text.line_visible = false;
    text.fx = ShapeEffects{};
    DrawShapeOnScreen(dl, text, r, scale);
  } else {
    ShapeObj plain = sh;
    plain.fx = ShapeEffects{};
    DrawShapeOnScreen(dl, plain, r, scale);
  }
  const int v_body = dl->VtxBuffer.Size;
  // ---- bevel (3-D Format): the fill lit from the top left, shaded towards the bottom right, a light inner rim ----
  if (fx.bevel_top || fx.bevel_bottom) {
    const float strength = std::clamp((fx.bevel_top ? fx.bevel_top_h : 0) + (fx.bevel_bottom ? fx.bevel_bottom_h : 0), 1.0f, 20.0f) / 20.0f;
    const ImU32 fill = sh.fill_visible ? WithAlpha(sh.fill_color, 1.0f) : 0;
    ScaleVerts(dl, v_shape, v_body, [&](ImDrawVert& v) {
      if (!fill || (v.col & ~IM_COL32_A_MASK) != (fill & ~IM_COL32_A_MASK)) return;  // only the fill (not the outline or text)
      const float t = ((v.pos.x - r.Min.x) / std::max(1.0f, r.GetWidth()) + (v.pos.y - r.Min.y) / std::max(1.0f, r.GetHeight())) * 0.5f;
      v.col = Brighten(v.col, 1.0f + (0.5f - t) * 0.9f * strength);
    });
    if (fx.bevel_top && sh.kind != model::ShapeKind::Line && sh.kind != model::ShapeKind::Arrow) {
      const float in = std::max(1.0f, fx.bevel_top_w * k * 0.5f);
      if (r.GetWidth() > 2 * in && r.GetHeight() > 2 * in) {
        ShapeObj rim = sh;
        rim.text.clear();
        rim.fill_visible = false;
        rim.line_visible = true;
        rim.line_color = Argb(IM_COL32(255, 255, 255, 110));
        rim.line_weight = std::max(0.75f, fx.bevel_top_w * 0.35f);
        rim.line_dash = 0;
        rim.fx = ShapeEffects{};
        DrawShapeOnScreen(dl, rim, ImRect(r.Min + ImVec2(in, in), r.Max - ImVec2(in, in)), scale);
      }
    }
  }
  // ---- inner shadow: the fill darkened on the side away from the light ----
  if (fx.shadow == 2) {
    const float a = fx.shadow_angle * 3.14159265f / 180.0f;
    const ImVec2 dir(-std::cos(a), -std::sin(a));
    const float dark = (1 - fx.shadow_transparency) * 0.8f;
    ScaleVerts(dl, v_shape, v_body, [&](ImDrawVert& v) {
      const float px = (v.pos.x - c.x) / std::max(1.0f, r.GetWidth() * 0.5f), py = (v.pos.y - c.y) / std::max(1.0f, r.GetHeight() * 0.5f);
      const float t = std::clamp((px * dir.x + py * dir.y + 1) * 0.5f, 0.0f, 1.0f);  // 1 on the shadowed side
      v.col = Brighten(v.col, 1.0f - dark * t * 0.6f);
    });
  }
  // ---- reflection: the shape mirrored below it, fading out over the reflected part ----
  if (fx.refl_size > 0) {
    const float gap = fx.refl_distance * k;
    const ImRect rr(ImVec2(r.Min.x, r.Max.y + gap), ImVec2(r.Max.x, r.Max.y + gap + r.GetHeight()));
    ShapeObj m = sh;
    m.flip_v = !sh.flip_v;
    m.text.clear();
    m.fx = ShapeEffects{};
    const int v0 = dl->VtxBuffer.Size;
    DrawShapeOnScreen(dl, m, rr, scale);
    const float reach = std::max(1.0f, r.GetHeight() * fx.refl_size / 100.0f);
    ScaleVerts(dl, v0, dl->VtxBuffer.Size, [&](ImDrawVert& v) {
      const float t = std::clamp((v.pos.y - rr.Min.y) / reach, 0.0f, 1.0f);
      const float alpha = (1 - fx.refl_transparency) * (1 - t);
      const float a0 = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF);
      v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a0 * alpha) << IM_COL32_A_SHIFT);
    });
  }
  // ---- 3-D rotation: everything drawn turned around the shape's center and seen in perspective ----
  if (fx.rot_x != 0 || fx.rot_y != 0 || fx.rot_z != 0 || fx.perspective != 0) {
    const float ax = fx.rot_x * 3.14159265f / 180.0f, ay = fx.rot_y * 3.14159265f / 180.0f, az = fx.rot_z * 3.14159265f / 180.0f;
    const float depth = std::max(r.GetWidth(), r.GetHeight()) * (fx.perspective > 0 ? 120.0f / fx.perspective : 1e6f);
    ScaleVerts(dl, v_all, dl->VtxBuffer.Size, [&](ImDrawVert& v) {
      float x = v.pos.x - c.x, y = v.pos.y - c.y;
      // z: in the screen plane
      float x1 = x * std::cos(az) - y * std::sin(az), y1 = x * std::sin(az) + y * std::cos(az);
      // x rotation turns left / right (around the vertical axis), y rotation tilts (around the horizontal axis)
      const float x2 = x1 * std::cos(ax), z2 = x1 * std::sin(ax);
      const float y3 = y1 * std::cos(ay) - z2 * std::sin(ay), z3 = y1 * std::sin(ay) + z2 * std::cos(ay);
      const float f = depth / std::max(1.0f, depth + z3);
      v.pos = ImVec2(c.x + x2 * f, c.y + y3 * f);
    });
  }
}

// ---- Format Shape > Effects (the pane) ----

namespace {

bool ColorBox(const char* label, uint32_t* argb) {
  float col[3] = {static_cast<float>((*argb >> 16) & 0xFF) / 255.0f, static_cast<float>((*argb >> 8) & 0xFF) / 255.0f, static_cast<float>(*argb & 0xFF) / 255.0f};
  if (!ImGui::ColorEdit3(label, col, ImGuiColorEditFlags_NoInputs)) return false;
  *argb = 0xFF000000u | (static_cast<uint32_t>(col[0] * 255 + 0.5f) << 16) | (static_cast<uint32_t>(col[1] * 255 + 0.5f) << 8) | static_cast<uint32_t>(col[2] * 255 + 0.5f);
  return true;
}

bool Percent(const char* label, float* v01) {
  int p = static_cast<int>(std::lround(*v01 * 100));
  ImGui::SetNextItemWidth(S(150));
  if (!ImGui::SliderInt(label, &p, 0, 100, "%d%%")) return false;
  *v01 = static_cast<float>(p) / 100.0f;
  return true;
}

bool Points(const char* label, float* v, float lo, float hi) {
  ImGui::SetNextItemWidth(S(150));
  return ImGui::DragFloat(label, v, 0.25f, lo, hi, "%.1f pt", ImGuiSliderFlags_AlwaysClamp);
}

bool Degrees(const char* label, float* v) {
  ImGui::SetNextItemWidth(S(150));
  if (!ImGui::DragFloat(label, v, 1.0f, -360.0f, 360.0f, "%.0f\xC2\xB0")) return false;
  *v = std::fmod(std::fmod(*v, 360.0f) + 360.0f, 360.0f);
  return true;
}

const char* const kBevels[] = {"None", "Circle", "Relaxed Inset", "Cross", "Cool Slant", "Angle", "Soft Round", "Convex", "Slope", "Divot", "Riblet", "Hard Edge", "Art Deco"};

}  // namespace

bool DrawShapeEffectsSection(ShapeObj& m) {
  bool changed = false;
  ShapeEffects& f = m.fx;
  ImGui::SeparatorText(TR("Effects"));
  ImGui::PushID("##effects");
  if (f.theme_style >= 0) {
    // a theme style's effects: editing one makes them the shape's own (Excel)
    if (ImGui::SmallButton(TR("Use the shape's own effects"))) {
      const int keep = f.theme_style;
      f = model::EffectiveEffects(f);
      f.theme_style = -1;
      (void)keep;
      changed = true;
    }
  }
  if (ImGui::TreeNode(TR("Shadow"))) {
    const char* types[] = {TR("No Shadow"), TR("Outer"), TR("Inner"), TR("Perspective")};
    ImGui::SetNextItemWidth(S(150));
    changed |= ImGui::Combo(TR("Presets##shadow"), &f.shadow, types, 4);
    ImGui::BeginDisabled(!f.shadow);
    changed |= ColorBox(TR("Color##shadow"), &f.shadow_color);
    changed |= Percent(TR("Transparency##shadow"), &f.shadow_transparency);
    ImGui::SetNextItemWidth(S(150));
    changed |= ImGui::DragFloat(TR("Size##shadow"), &f.shadow_size, 1.0f, 1.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    changed |= Points(TR("Blur##shadow"), &f.shadow_blur, 0, 100);
    changed |= Degrees(TR("Angle##shadow"), &f.shadow_angle);
    changed |= Points(TR("Distance##shadow"), &f.shadow_distance, 0, 200);
    ImGui::EndDisabled();
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(TR("Reflection"))) {
    ImGui::SetNextItemWidth(S(150));
    changed |= ImGui::DragFloat(TR("Size##refl"), &f.refl_size, 1.0f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::BeginDisabled(f.refl_size <= 0);
    changed |= Percent(TR("Transparency##refl"), &f.refl_transparency);
    changed |= Percent(TR("Blur##refl"), &f.refl_blur);
    changed |= Points(TR("Distance##refl"), &f.refl_distance, 0, 100);
    ImGui::EndDisabled();
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(TR("Glow"))) {
    changed |= Points(TR("Size##glow"), &f.glow_size, 0, 150);
    ImGui::BeginDisabled(f.glow_size <= 0);
    changed |= ColorBox(TR("Color##glow"), &f.glow_color);
    changed |= Percent(TR("Transparency##glow"), &f.glow_transparency);
    ImGui::EndDisabled();
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(TR("Soft Edges"))) {
    changed |= Points(TR("Size##soft"), &f.soft_edge, 0, 100);
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(TR("3-D Format"))) {
    std::vector<const char*> names;
    for (const char* b : kBevels) names.push_back(TR(b));
    ImGui::SetNextItemWidth(S(150));
    changed |= ImGui::Combo(TR("Top bevel"), &f.bevel_top, names.data(), static_cast<int>(names.size()));
    changed |= Points(TR("Width##top"), &f.bevel_top_w, 0, 100);
    changed |= Points(TR("Height##top"), &f.bevel_top_h, 0, 100);
    ImGui::SetNextItemWidth(S(150));
    changed |= ImGui::Combo(TR("Bottom bevel"), &f.bevel_bottom, names.data(), static_cast<int>(names.size()));
    changed |= Points(TR("Width##bottom"), &f.bevel_bottom_w, 0, 100);
    changed |= Points(TR("Height##bottom"), &f.bevel_bottom_h, 0, 100);
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(TR("3-D Rotation"))) {
    const auto& rs = model::Rotations3D();
    const char* cur = f.rot_preset > 0 && f.rot_preset <= static_cast<int>(rs.size()) ? TR(rs[static_cast<size_t>(f.rot_preset - 1)].name) : TR("No Rotation");
    ImGui::SetNextItemWidth(S(150));
    if (ImGui::BeginCombo(TR("Presets##rot"), cur)) {
      if (ImGui::Selectable(TR("No Rotation"), f.rot_preset == 0)) f.rot_preset = 0, f.rot_x = f.rot_y = f.rot_z = f.perspective = 0, changed = true;
      for (size_t i = 0; i < rs.size(); ++i) {
        if (ImGui::Selectable(TR(rs[i].name), f.rot_preset == static_cast<int>(i + 1))) {
          f.rot_preset = static_cast<int>(i + 1), f.rot_x = rs[i].x, f.rot_y = rs[i].y, f.rot_z = rs[i].z, f.perspective = rs[i].perspective;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    changed |= Degrees(TR("X Rotation"), &f.rot_x);
    changed |= Degrees(TR("Y Rotation"), &f.rot_y);
    changed |= Degrees(TR("Z Rotation"), &f.rot_z);
    ImGui::SetNextItemWidth(S(150));
    changed |= ImGui::DragFloat(TR("Perspective"), &f.perspective, 1.0f, 0.0f, 120.0f, "%.0f\xC2\xB0", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TreePop();
  }
  ImGui::PopID();
  if (changed) f.preset = 0;  // edited by hand: no gallery entry is checked
  return changed;
}

// ---- Shape Format > Shape Effects / Shape Styles; Page Layout > Themes > Effects ----

namespace {

// the effects each gallery entry gives (false: not one of them)
bool FxOf(const std::string& id, ShapeEffects* out, std::string* label) {
  auto num = [&](const char* prefix, int* k) {
    const size_t n = std::char_traits<char>::length(prefix);
    if (id.compare(0, n, prefix) != 0 || id.size() == n || !std::isdigit(static_cast<unsigned char>(id[n]))) return false;
    *k = std::atoi(id.c_str() + n);
    return true;
  };
  int k = 0;
  ShapeEffects e;
  char b[96] = "";
  if (num("ShapeFxPreset", &k)) {
    e = model::PresetEffects(k);
    if (k == 0) std::snprintf(b, sizeof b, "%s", TR("No Presets"));
    else std::snprintf(b, sizeof b, TR("Preset %d"), k);
  } else if (num("ShapeFxShadow", &k)) {
    // 1-9 outer, 10-18 inner (offsets around the clock from bottom right), 19-22 perspective
    static const float kAngles[] = {45, 90, 135, 0, 0, 180, 315, 270, 225};
    static const char* const kNames[] = {"Offset: Bottom Right", "Offset: Bottom", "Offset: Bottom Left", "Offset: Right", "Offset: Center",
                                         "Offset: Left",          "Offset: Top Right", "Offset: Top",       "Offset: Top Left"};
    if (k >= 1 && k <= 18) {
      const int i = (k - 1) % 9;
      e.shadow = k <= 9 ? 1 : 2;
      e.shadow_angle = kAngles[i];
      e.shadow_distance = i == 4 ? 0.0f : 3.0f;
      e.shadow_blur = i == 4 ? 5.0f : 4.0f;
      e.shadow_transparency = 0.6f;
      std::snprintf(b, sizeof b, "%s: %s", k <= 9 ? TR("Outer") : TR("Inner"), TR(kNames[i]));
    } else if (k >= 19 && k <= 22) {
      static const float kPa[] = {90, 225, 315, 135};
      static const char* const kPn[] = {"Perspective: Below", "Perspective: Upper Left", "Perspective: Upper Right", "Perspective: Lower Left"};
      e.shadow = 3;
      e.shadow_angle = kPa[k - 19];
      e.shadow_distance = 4;
      e.shadow_blur = 6;
      e.shadow_transparency = 0.6f;
      std::snprintf(b, sizeof b, "%s", TR(kPn[k - 19]));
    } else {
      std::snprintf(b, sizeof b, "%s", TR("No Shadow"));
    }
  } else if (num("ShapeFxReflection", &k)) {
    // tight / half / full reflection, touching / 4 pt / 8 pt offset
    if (k >= 1 && k <= 9) {
      static const float kSizes[] = {35, 55, 90};
      static const char* const kNames[] = {"Tight Reflection", "Half Reflection", "Full Reflection"};
      e.refl_size = kSizes[(k - 1) % 3];
      e.refl_distance = static_cast<float>(((k - 1) / 3) * 4);
      e.refl_transparency = 0.5f;
      std::snprintf(b, sizeof b, "%s, %s", TR(kNames[(k - 1) % 3]), k <= 3 ? TR("touching") : k <= 6 ? TR("4 pt offset") : TR("8 pt offset"));
    } else {
      std::snprintf(b, sizeof b, "%s", TR("No Reflection"));
    }
  } else if (num("ShapeFxGlow", &k)) {
    // 5, 8, 11, 18 pt in the theme's six accents
    if (k >= 1 && k <= 24) {
      static const float kSizes[] = {5, 8, 11, 18};
      const int accent = (k - 1) % 6, size = (k - 1) / 6;
      e.glow_size = kSizes[size];
      e.glow_color = 0xFF000000u | model::CurrentTheme().colors[static_cast<size_t>(4 + accent)];
      e.glow_transparency = 0.6f;
      std::snprintf(b, sizeof b, TR("Glow: %g pt; Accent color %d"), kSizes[size], accent + 1);
    } else {
      std::snprintf(b, sizeof b, "%s", TR("No Glow"));
    }
  } else if (num("ShapeFxSoft", &k)) {
    static const float kPts[] = {0, 1, 2.5f, 5, 10, 25, 50};
    if (k < 0 || k > 6) return false;
    e.soft_edge = kPts[k];
    if (k == 0) std::snprintf(b, sizeof b, "%s", TR("No Soft Edges"));
    else std::snprintf(b, sizeof b, "%g %s", kPts[k], TR("Point"));
  } else if (num("ShapeFxBevel", &k)) {
    if (k < 0 || k > 12) return false;
    e.bevel_top = k;
    e.bevel_top_w = e.bevel_top_h = 6;
    std::snprintf(b, sizeof b, "%s", k == 0 ? TR("No Bevel") : TR(kBevels[k]));
  } else if (num("ShapeFx3D", &k)) {
    const auto& rs = model::Rotations3D();
    if (k < 0 || k > static_cast<int>(rs.size())) return false;
    if (k > 0) {
      const auto& q = rs[static_cast<size_t>(k - 1)];
      e.rot_preset = k, e.rot_x = q.x, e.rot_y = q.y, e.rot_z = q.z, e.perspective = q.perspective;
    }
    std::snprintf(b, sizeof b, "%s", k == 0 ? TR("No Rotation") : TR(rs[static_cast<size_t>(k - 1)].name));
  } else {
    return false;
  }
  *out = e;
  if (label) *label = b;
  return true;
}

// the effect a gallery entry changes (the others stay): 0 preset (all), 1 shadow, 2 reflection, 3 glow, 4 soft, 5
// bevel, 6 3-D rotation
int FxPart(const std::string& id) {
  static const char* const kPre[] = {"ShapeFxPreset", "ShapeFxShadow", "ShapeFxReflection", "ShapeFxGlow", "ShapeFxSoft", "ShapeFxBevel", "ShapeFx3D"};
  for (int i = 0; i < 7; ++i) {
    if (id.rfind(kPre[i], 0) == 0) return i;
  }
  return -1;
}

void MergeFx(ShapeEffects& f, const ShapeEffects& e, int part) {
  if (part != 0 && f.theme_style >= 0) {  // a theme style's effects become the shape's own before one part changes
    f = model::EffectiveEffects(f);
    f.theme_style = -1;
  }
  switch (part) {
    case 0: f = e; break;
    case 1:
      f.shadow = e.shadow, f.shadow_angle = e.shadow_angle, f.shadow_distance = e.shadow_distance, f.shadow_blur = e.shadow_blur,
      f.shadow_transparency = e.shadow_transparency, f.shadow_size = 100;
      break;
    case 2: f.refl_size = e.refl_size, f.refl_distance = e.refl_distance, f.refl_transparency = e.refl_transparency; break;
    case 3: f.glow_size = e.glow_size, f.glow_color = e.glow_color, f.glow_transparency = e.glow_transparency; break;
    case 4: f.soft_edge = e.soft_edge; break;
    case 5: f.bevel_top = e.bevel_top, f.bevel_top_w = e.bevel_top_w, f.bevel_top_h = e.bevel_top_h; break;
    case 6: f.rot_preset = e.rot_preset, f.rot_x = e.rot_x, f.rot_y = e.rot_y, f.rot_z = e.rot_z, f.perspective = e.perspective; break;
    default: break;
  }
  if (part != 0) f.preset = 0;
}

std::vector<uint32_t> SelectedShapes() {
  std::vector<uint32_t> ids;
  for (const auto& [kind, id] : SelectedObjectIds()) {
    if (kind == 3) ids.push_back(id);
  }
  return ids;
}

void ApplyToShapes(AppState& s, const char* label, std::function<void(ShapeObj&)> fn) {
  const uint32_t sheet = s.focus_sheet;
  const std::vector<uint32_t> ids = SelectedShapes();
  if (ids.empty()) return;
  s.engine->PostUser(label, [sheet, ids, fn](Document& d) {
    for (uint32_t id : ids) d.UpdateShape(sheet, id, fn);
  });
}

// Shape Styles' Theme Styles: rows (Colored Outline, Colored Fill, Light 1 Outline Colored Fill, Subtle / Moderate /
// Intense Effect) by color (Dark 1, Accent 1-6)
void QuickStyle(int k, ShapeObj& x) {
  const int row = k / 7, col = k % 7;
  const auto& th = model::CurrentTheme();
  const uint32_t base = 0xFF000000u | th.colors[static_cast<size_t>(col == 0 ? 0 : 3 + col)];
  auto darker = [](uint32_t c) {
    return 0xFF000000u | ((((c >> 16) & 0xFF) * 3 / 4) << 16) | ((((c >> 8) & 0xFF) * 3 / 4) << 8) | ((c & 0xFF) * 3 / 4);
  };
  x.fill_visible = true;
  x.line_visible = true;
  x.fill_transparency = 0;
  x.line_weight = row == 2 ? 1.0f : 0.75f;
  if (row == 0) x.fill_color = 0xFFFFFFFF, x.line_color = base, x.font_color = 0xFF000000;
  else if (row == 1) x.fill_color = base, x.line_color = darker(base), x.font_color = 0xFFFFFFFF;
  else if (row == 2) x.fill_color = base, x.line_color = 0xFFFFFFFF, x.font_color = 0xFFFFFFFF;
  else x.fill_color = base, x.line_visible = row == 3, x.line_color = darker(base), x.font_color = 0xFFFFFFFF;
  // the effect: the theme's subtle / moderate / intense effect style (Page Layout > Themes > Effects changes it)
  x.fx = ShapeEffects{};
  x.fx.theme_style = row <= 3 ? 0 : row - 3;
}

// Page Layout > Themes > Effects: the scheme's name in the workbook store (view.theme_effects)
std::string StoredScheme(const model::Workbook& wb) {
  auto it = wb.store->find("view.theme_effects");
  if (it == wb.store->end()) return "Office";
  try {
    const nlohmann::json j = nlohmann::json::from_msgpack(it->second);
    return j.is_string() ? j.get<std::string>() : std::string("Office");
  } catch (...) {
    return "Office";
  }
}

int SchemeIndex(const std::string& name) {
  const auto& all = model::BuiltinThemeEffects();
  for (size_t i = 0; i < all.size(); ++i) {
    if (name == all[i].name) return static_cast<int>(i);
  }
  return -1;
}

bool g_scheme_preview = false;  // a Themes > Effects entry under the mouse: its scheme is shown (live preview)

float FxMenuPicture(AppState& s, const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) {
  (void)s;
  ShapeEffects e;
  int scheme = -1, quick = -1;
  if (cmd.rfind("ThemeEffects", 0) == 0 && cmd.size() > 12 && std::isdigit(static_cast<unsigned char>(cmd[12]))) {
    scheme = std::atoi(cmd.c_str() + 12);
    if (scheme < 0 || scheme >= static_cast<int>(model::BuiltinThemeEffects().size())) return -1;
  } else if (cmd.rfind("ShapeStyle", 0) == 0 && cmd.size() > 10 && std::isdigit(static_cast<unsigned char>(cmd[10]))) {
    quick = std::atoi(cmd.c_str() + 10);
  } else if (!FxOf(cmd, &e, nullptr)) {
    return -1;
  }
  const float side = std::max(S(20), h);
  if (!dl) return side;
  ShapeObj sample;
  sample.kind = model::ShapeKind::RoundedRectangle;
  sample.fill_color = 0xFF000000u | model::CurrentTheme().colors[4];
  sample.line_color = 0xFF2F528F;
  if (quick >= 0) QuickStyle(quick, sample);
  if (scheme >= 0) {
    sample.fx = model::BuiltinThemeEffects()[static_cast<size_t>(scheme)].styles[2];
    if (ImGui::IsItemHovered()) {  // live preview: the sheet's shapes drawn with this scheme
      model::CurrentThemeEffects().store(scheme);
      g_scheme_preview = true;
    }
  } else if (quick < 0) {
    sample.fx = e;
  }
  const float m = side * 0.22f;
  const ImRect r(ImVec2(at.x + m, at.y + m), ImVec2(at.x + side - m, at.y + side - m * 1.4f));
  const ShapeEffects keep = sample.fx;
  if (scheme >= 0) {
    // the scheme's intense style, drawn as such (not through the workbook's current scheme)
    sample.fx.theme_style = -1;
  }
  (void)keep;
  DrawShapeFx(dl, sample, r, side / S(70));
  return side;
}

template <typename F>
void ForItem(const std::string& cmd, F fn) {
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == cmd) fn(it);
        }
        for (auto& row : b.rows) {
          for (auto& it : row) {
            if (it.cmd == cmd) fn(it);
          }
        }
      }
    }
  }
}

}  // namespace

void ApplyShapeQuickStyle(AppState& s, uint32_t sheet, uint32_t id, int k) {
  if (k < 0 || k >= 42) return;
  s.engine->PostUser(TR("Shape Styles"), [sheet, id, k](Document& d) { d.UpdateShape(sheet, id, [k](ShapeObj& x) { QuickStyle(k, x); }); });
}

std::string ThemeEffectsName(AppState& s) { return s.snap ? StoredScheme(s.snap->wb) : std::string("Office"); }

bool SetThemeEffects(AppState& s, const std::string& name) {
  if (SchemeIndex(name) < 0) return false;
  const std::vector<uint8_t> b = nlohmann::json::to_msgpack(nlohmann::json(name));
  const std::string bytes(b.begin(), b.end());
  s.engine->PostUser(TR("Theme Effects"), [bytes](Document& d) { d.SetThemeEffects(bytes); });
  return true;
}

void RegisterShapeEffectCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const std::string& id, const std::string& label, const char* icon = nullptr, const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("ShapeEffectsMenu", TR("Shape Effects"), ui::icon::Effects, TR("Apply a visual effect to the shape: shadow, glow, reflection, 3-D rotation."))).execute =
      [](ui::Command&) {};
  struct Head {
    const char* id;
    const char* label;
  };
  for (const Head& h : {Head{"ShapeFxPresetMenu", TR("Preset")}, Head{"ShapeFxShadowMenu", TR("Shadow")}, Head{"ShapeFxReflectionMenu", TR("Reflection")},
                        Head{"ShapeFxGlowMenu", TR("Glow")}, Head{"ShapeFxSoftMenu", TR("Soft Edges")}, Head{"ShapeFxBevelMenu", TR("Bevel")},
                        Head{"ShapeFx3DMenu", TR("3-D Rotation")}}) {
    R.Add(make(h.id, h.label)).execute = [](ui::Command&) {};
  }
  auto add_fx = [&](const std::string& id) {
    ShapeEffects e;
    std::string label;
    FxOf(id, &e, &label);
    const int part = FxPart(id);
    R.Add(make(id, label)).execute = [&s, id, part](ui::Command&) {
      ShapeEffects fx;
      FxOf(id, &fx, nullptr);  // now (the glow takes the theme's accents of now)
      ApplyToShapes(s, TR("Shape Effects"), [fx, part](ShapeObj& x) { MergeFx(x.fx, fx, part); });
    };
  };
  for (int k = 0; k <= 12; ++k) add_fx("ShapeFxPreset" + std::to_string(k));
  for (int k = 0; k <= 22; ++k) add_fx("ShapeFxShadow" + std::to_string(k));
  for (int k = 0; k <= 9; ++k) add_fx("ShapeFxReflection" + std::to_string(k));
  for (int k = 0; k <= 24; ++k) add_fx("ShapeFxGlow" + std::to_string(k));
  for (int k = 0; k <= 6; ++k) add_fx("ShapeFxSoft" + std::to_string(k));
  for (int k = 0; k <= 12; ++k) add_fx("ShapeFxBevel" + std::to_string(k));
  for (int k = 0; k <= static_cast<int>(model::Rotations3D().size()); ++k) add_fx("ShapeFx3D" + std::to_string(k));
  // Shape Styles (Theme Styles)
  R.Add(make("ShapeQuickStylesMenu", TR("Quick Styles"), ui::icon::Brush, TR("Choose a visual style for the shape: its fill, outline and the theme's effects."))).execute =
      [](ui::Command&) {};
  static const char* const kRows[] = {"Colored Outline", "Colored Fill", "Light 1 Outline, Colored Fill", "Subtle Effect", "Moderate Effect", "Intense Effect"};
  static const char* const kCols[] = {"Black, Dark 1", "Blue, Accent 1", "Orange, Accent 2", "Gray, Accent 3", "Gold, Accent 4", "Blue, Accent 5", "Green, Accent 6"};
  for (int k = 0; k < 42; ++k) {
    const std::string label = std::string(TR(kRows[k / 7])) + " - " + TR(kCols[k % 7]);
    R.Add(make("ShapeStyle" + std::to_string(k), label)).execute = [&s, k](ui::Command&) {
      ApplyToShapes(s, TR("Shape Styles"), [k](ShapeObj& x) { QuickStyle(k, x); });
    };
  }
  // Page Layout > Themes > Effects
  R.Add(make("ThemeEffectsMenu", TR("Effects"), ui::icon::Effects, TR("Change the general look of the shapes that use the theme's effect styles."))).execute =
      [](ui::Command&) {};
  const auto& schemes = model::BuiltinThemeEffects();
  for (size_t i = 0; i < schemes.size(); ++i) {
    const std::string name = schemes[i].name;
    R.Add(make("ThemeEffects" + std::to_string(i), name)).execute = [&s, name](ui::Command&) { SetThemeEffects(s, name); };
  }
}

// every frame: the effect scheme the shapes are drawn with; the menus' entries (once)
void UpdateShapeEffects(AppState& s) {
  static bool once = false;
  if (!once) {
    once = true;
    auto& st = ui::ribbon::St();
    st.menu_picture = [&s, prev = st.menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
      const float w = FxMenuPicture(s, cmd, dl, at, h);
      if (w >= 0) return w;
      return prev ? prev(cmd, dl, at, h) : 0.0f;
    };
    // the Shape Effects menu: a submenu per effect, its entries as pictures (Excel's galleries)
    auto tiles = [](const char* prefix, int from, int to) {
      std::vector<std::string> v;
      for (int k = from; k <= to; ++k) v.push_back(prefix + std::to_string(k));
      return v;
    };
    std::vector<std::string> menu;
    auto sub = [&](const char* head, std::vector<std::pair<std::string, std::vector<std::string>>> sections, const std::string& none) {
      menu.push_back(std::string(">") + head);
      if (!none.empty()) menu.push_back(none);
      for (auto& [title, ids] : sections) {
        menu.push_back("=" + title);
        menu.insert(menu.end(), ids.begin(), ids.end());
      }
      menu.push_back("<");
    };
    sub("ShapeFxPresetMenu", {{TR("Presets"), tiles("ShapeFxPreset", 1, 12)}}, "ShapeFxPreset0");
    sub("ShapeFxShadowMenu", {{TR("Outer"), tiles("ShapeFxShadow", 1, 9)}, {TR("Inner"), tiles("ShapeFxShadow", 10, 18)}, {TR("Perspective"), tiles("ShapeFxShadow", 19, 22)}},
        "ShapeFxShadow0");
    sub("ShapeFxReflectionMenu", {{TR("Reflection Variations"), tiles("ShapeFxReflection", 1, 9)}}, "ShapeFxReflection0");
    sub("ShapeFxGlowMenu", {{TR("Glow Variations"), tiles("ShapeFxGlow", 1, 24)}}, "ShapeFxGlow0");
    sub("ShapeFxSoftMenu", {{TR("Soft Edge Variations"), tiles("ShapeFxSoft", 1, 6)}}, "ShapeFxSoft0");
    sub("ShapeFxBevelMenu", {{TR("Bevel"), tiles("ShapeFxBevel", 1, 12)}}, "ShapeFxBevel0");
    sub("ShapeFx3DMenu", {{TR("Parallel"), tiles("ShapeFx3D", 1, 8)}, {TR("Perspective"), tiles("ShapeFx3D", 9, 14)}, {TR("Oblique"), tiles("ShapeFx3D", 15, 16)}},
        "ShapeFx3D0");
    ForItem("ShapeEffectsMenu", [&](ui::ribbon::Item& it) { it.menu = menu; });
    std::vector<std::string> styles{std::string("=") + TR("Theme Styles")};
    for (int k = 0; k < 42; ++k) styles.push_back("ShapeStyle" + std::to_string(k));
    ForItem("ShapeQuickStylesMenu", [&](ui::ribbon::Item& it) { it.menu = styles; });
    std::vector<std::string> schemes;
    for (size_t i = 0; i < model::BuiltinThemeEffects().size(); ++i) schemes.push_back("ThemeEffects" + std::to_string(i));
    ForItem("ThemeEffectsMenu", [&](ui::ribbon::Item& it) { it.menu = schemes; });
  }
  // the workbook's scheme (a live preview lasts while its entry is under the mouse)
  g_scheme_preview = false;
  if (!s.snap) return;
  const std::string name = StoredScheme(s.snap->wb);
  const int i = std::max(0, SchemeIndex(name));
  model::CurrentThemeEffects().store(i);
  for (size_t k = 0; k < model::BuiltinThemeEffects().size(); ++k) {
    if (auto* c = ui::Commands().Find("ThemeEffects" + std::to_string(k))) c->toggle = true, c->pressed = static_cast<int>(k) == i;
  }
  // the gallery entry of the selected shape's preset shows checked
  int preset = -1;
  if (const model::Sheet* sh = s.FocusSheet()) {
    const auto ids = SelectedShapes();
    for (const auto& x : *sh->shapes) {
      if (!ids.empty() && x.id == ids[0]) preset = x.fx.preset;
    }
  }
  for (int k = 0; k <= 12; ++k) {
    if (auto* c = ui::Commands().Find("ShapeFxPreset" + std::to_string(k))) c->toggle = true, c->pressed = preset == k;
  }
}

}  // namespace yc::app
