// SPDX-License-Identifier: Apache-2.0
// shape_effects.h - Format Shape > Effects (Shadow, Reflection, Glow, Soft Edges, 3-D Format, 3-D Rotation) and the
// theme's effect styles (Page Layout > Themes > Effects) that shapes with a theme style take
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace yc::model {

struct ShapeEffects {
  int preset = 0;        // Shape Effects > Preset chosen last (1-12, 0 none): what the gallery shows checked
  int theme_style = -1;  // the theme's effect style the shape takes (0 subtle, 1 moderate, 2 intense); -1: its own below
  // Shadow: 0 none, 1 outer, 2 inner, 3 perspective (below, flattened)
  int shadow = 0;
  uint32_t shadow_color = 0xFF000000;  // ARGB
  float shadow_transparency = 0.6f;    // 0 .. 1
  float shadow_size = 100;             // percent of the shape
  float shadow_blur = 4;               // points
  float shadow_angle = 45;             // degrees (0 = to the right, 90 = down)
  float shadow_distance = 3;           // points
  // Reflection: the shape mirrored below it, fading out
  float refl_size = 0;  // percent of the shape reflected (0 = none)
  float refl_transparency = 0.5f, refl_blur = 0.5f, refl_distance = 0;
  // Glow around the outline
  float glow_size = 0;  // points (0 = none)
  uint32_t glow_color = 0xFF4472C4;
  float glow_transparency = 0.6f;
  float soft_edge = 0;  // Soft Edges: points feathered (0 = none)
  // 3-D Format: bevel (0 none, 1 circle, 2 relaxed inset, 3 cross, 4 cool slant, 5 angle, 6 soft round, 7 convex,
  // 8 slope, 9 divot, 10 riblet, 11 hard edge, 12 art deco) and its width / height in points
  int bevel_top = 0, bevel_bottom = 0;
  float bevel_top_w = 6, bevel_top_h = 6, bevel_bottom_w = 6, bevel_bottom_h = 6;
  // 3-D Rotation: degrees around the x (left-right turn), y (tilt) and z axes, and the perspective (field of view)
  float rot_x = 0, rot_y = 0, rot_z = 0, perspective = 0;
  int rot_preset = 0;  // the 3-D Rotation gallery's entry (0 none)

  bool Any() const {
    return theme_style >= 0 || shadow || refl_size > 0 || glow_size > 0 || soft_edge > 0 || bevel_top || bevel_bottom || rot_x != 0 || rot_y != 0 ||
           rot_z != 0 || perspective != 0;
  }
  bool operator==(const ShapeEffects&) const = default;
};

// Page Layout > Themes > Effects: a scheme's three effect styles (subtle, moderate, intense), as Office's themes
struct ThemeEffectScheme {
  const char* name;
  std::array<ShapeEffects, 3> styles;
};

inline const std::vector<ThemeEffectScheme>& BuiltinThemeEffects() {
  static const std::vector<ThemeEffectScheme> v = [] {
    auto none = ShapeEffects{};
    auto shadow = [](float dist, float blur, float transparency, float angle = 90) {
      ShapeEffects e;
      e.shadow = 1;
      e.shadow_distance = dist;
      e.shadow_blur = blur;
      e.shadow_transparency = transparency;
      e.shadow_angle = angle;
      return e;
    };
    auto bevel = [](ShapeEffects e, int type, float w) {
      e.bevel_top = type;
      e.bevel_top_w = e.bevel_top_h = w;
      return e;
    };
    auto glow = [](ShapeEffects e, float size) {
      e.glow_size = size;
      e.glow_transparency = 0.6f;
      return e;
    };
    auto refl = [](ShapeEffects e, float size) {
      e.refl_size = size;
      e.refl_transparency = 0.5f;
      e.refl_distance = 1;
      return e;
    };
    auto soft = [](ShapeEffects e, float pt) {
      e.soft_edge = pt;
      return e;
    };
    std::vector<ThemeEffectScheme> s;
    s.push_back({"Office", {none, none, shadow(3, 4, 0.37f)}});
    s.push_back({"Office 2007 - 2010", {shadow(1.5f, 3, 0.65f), shadow(2, 4, 0.62f), bevel(shadow(2, 4, 0.6f), 1, 4)}});
    s.push_back({"Subtle Solids", {none, shadow(1, 1.5f, 0.7f), shadow(2, 3, 0.6f)}});
    s.push_back({"Banded Edges", {none, bevel(none, 5, 3), bevel(shadow(2, 3, 0.6f), 5, 5)}});
    s.push_back({"Smokey Glass", {none, shadow(2, 4, 0.7f), refl(shadow(3, 6, 0.6f), 30)}});
    s.push_back({"Glow Edge", {none, glow(none, 3), glow(none, 6)}});
    s.push_back({"Grunge Texture", {none, soft(none, 1.5f), soft(shadow(3, 6, 0.55f), 2)}});
    s.push_back({"Frosted Glass", {none, bevel(none, 6, 3), bevel(glow(none, 4), 6, 6)}});
    s.push_back({"Top Shadow", {shadow(1, 2, 0.6f), shadow(2, 4, 0.55f), shadow(4, 8, 0.5f)}});
    s.push_back({"Inset", {none, bevel(none, 2, 3), bevel(shadow(2, 3, 0.6f), 2, 6)}});
    s.push_back({"Milk Glass", {none, bevel(none, 1, 3), refl(bevel(none, 1, 6), 40)}});
    s.push_back({"Riblet", {none, bevel(none, 10, 3), bevel(shadow(3, 4, 0.6f), 10, 6)}});
    s.push_back({"Reflection", {none, refl(none, 25), refl(shadow(2, 4, 0.6f), 45)}});
    s.push_back({"Extreme Shadow", {shadow(3, 3, 0.5f, 45), shadow(6, 6, 0.45f, 45), shadow(9, 10, 0.4f, 45)}});
    s.push_back({"Glossy", {none, bevel(none, 7, 4), refl(bevel(shadow(2, 4, 0.6f), 7, 6), 30)}});
    return s;
  }();
  return v;
}

// the theme's effect scheme the shapes are drawn with (index into BuiltinThemeEffects; the app sets it from the
// workbook, any thread reads it)
inline std::atomic<int>& CurrentThemeEffects() {
  static std::atomic<int> v{0};
  return v;
}

// the effects a shape is drawn with: its theme style's from the current scheme, or its own
inline ShapeEffects EffectiveEffects(const ShapeEffects& own) {
  if (own.theme_style < 0 || own.theme_style > 2) return own;
  const auto& all = BuiltinThemeEffects();
  const int i = CurrentThemeEffects().load();
  ShapeEffects e = all[static_cast<size_t>(i >= 0 && i < static_cast<int>(all.size()) ? i : 0)].styles[static_cast<size_t>(own.theme_style)];
  // the shape's own 3-D rotation stays (a theme style brings shadows, glows, bevels and reflections)
  e.rot_x = own.rot_x, e.rot_y = own.rot_y, e.rot_z = own.rot_z, e.perspective = own.perspective, e.rot_preset = own.rot_preset;
  e.theme_style = own.theme_style;
  return e;
}

// Shape Effects > Preset 1-12 (Excel's gallery: shadows, bevels and their mixes)
inline ShapeEffects PresetEffects(int k) {
  ShapeEffects e;
  e.preset = k;
  auto sh = [&](float d, float b, float t) {
    e.shadow = 1, e.shadow_distance = d, e.shadow_blur = b, e.shadow_transparency = t, e.shadow_angle = 90;
  };
  switch (k) {
    case 1: sh(3, 4, 0.6f); break;
    case 2: sh(3, 4, 0.6f), e.bevel_top = 1, e.bevel_top_w = e.bevel_top_h = 6; break;
    case 3: e.bevel_top = 1, e.bevel_top_w = e.bevel_top_h = 6; break;
    case 4: e.bevel_top = 2, e.bevel_top_w = e.bevel_top_h = 6; break;
    case 5: sh(3, 4, 0.6f), e.bevel_top = 6, e.bevel_top_w = e.bevel_top_h = 8; break;
    case 6: e.bevel_top = 7, e.bevel_top_w = e.bevel_top_h = 8; break;
    case 7: sh(2, 3, 0.6f), e.bevel_top = 8, e.bevel_top_w = e.bevel_top_h = 6; break;
    case 8: e.bevel_top = 5, e.bevel_top_w = e.bevel_top_h = 4; break;
    case 9: sh(4, 6, 0.55f), e.bevel_top = 1, e.bevel_top_w = e.bevel_top_h = 10; break;
    case 10: e.bevel_top = 11, e.bevel_top_w = e.bevel_top_h = 6, e.rot_x = 20, e.rot_y = 10, e.perspective = 30; break;
    case 11: sh(3, 6, 0.6f), e.bevel_top = 12, e.bevel_top_w = e.bevel_top_h = 6, e.rot_x = 320, e.rot_y = 10, e.perspective = 30; break;
    case 12: e.bevel_top = 6, e.bevel_top_w = e.bevel_top_h = 6, e.rot_y = 300, e.perspective = 40; break;
    default: e.preset = 0; break;
  }
  return e;
}

// 3-D Rotation gallery entries: Parallel (Isometric Left Down, Right Up, Off Axis 1 Left / Right, Off Axis 2...),
// Perspective (Front, Left, Right, Below, Above...), Oblique: x, y, z rotation and perspective
struct Rotation3D {
  const char* name;
  float x, y, z, perspective;
};
inline const std::vector<Rotation3D>& Rotations3D() {
  static const std::vector<Rotation3D> v = {
      {"Isometric Left Down", 45, 35, 0, 0},   {"Isometric Right Up", 315, 35, 0, 0},     {"Isometric Top Up", 315, 325, 0, 0},
      {"Isometric Bottom Down", 45, 325, 0, 0}, {"Off Axis 1 Left", 64, 18, 0, 0},       {"Off Axis 1 Right", 296, 18, 0, 0},
      {"Off Axis 2 Left", 64, 342, 0, 0},       {"Off Axis 2 Right", 296, 342, 0, 0},     {"Perspective Front", 0, 0, 0, 45},
      {"Perspective Left", 20, 0, 0, 45},       {"Perspective Right", 340, 0, 0, 45},     {"Perspective Below", 0, 20, 0, 45},
      {"Perspective Above", 0, 340, 0, 45},     {"Perspective Relaxed", 0, 309, 0, 45},   {"Oblique Top Left", 30, 330, 0, 0},
      {"Oblique Bottom Right", 330, 30, 0, 0},
  };
  return v;
}

}  // namespace yc::model
