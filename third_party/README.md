# third_party: third-party dependencies and version pinning

## Version matrix (single source: [`deps.lock`](deps.lock))

| Package | Source | Version | commit | Notes |
|---|---|---|---|---|
| Dear ImGui | git submodule `third_party/imgui` | `v1.93.0-docking` (`IMGUI_VERSION_NUM 19300`) | `b916186e66aa` | docking branch, 2026-10-14 |
| ImPlot | git submodule `third_party/implot` | master, "1.1 WIP" (`IMPLOT_VERSION_NUM 10100`) | `851cdbdd60d1` | 2026-10-10 |
| Fluent UI System Icons | committed directly in `fluent-icons/` (font, codepoint map, LICENSE) | — | `463d58b8186f` | MIT; embedded in ycgrid.exe |
| FreeType, nlohmann-json, zstd, zlib, Catch2, pugixml | vcpkg manifest (`vcpkg.json`) | baseline `2026.10.16` | `9eec002df1cd` | statically linked (x64-windows-static) |
| Natural Earth (admin 0 countries 1:50m, admin 1 states and provinces 1:10m) | generated into `src/engine/geo_data.inc` by `scripts/build_map_data.py` | 5.x (2026) | — | public domain; the shapes of filled map charts ("Made with Natural Earth") |
| uv, CPython | downloaded by `scripts\package.cmd --with-python` | uv 0.12.24, CPython 3.12.15 | — | only used for packaging |

## Why ImPlot is pinned to master instead of the v1.0 release

ImPlot v1.0 (2026-04-05) is only compatible up to about ImGui 1.92.7. ImGui made three breaking changes since then, and the ImPlot fixes are only on master:

| ImGui change | Impact | Fix on ImPlot master |
|---|---|---|
| 1.92.8: `ImDrawList::AddRect()` swapped the `thickness` and `flags` parameters, the old signature is `= delete` | compile error | 9751e23 |
| 1.93: `AddLine()` dropped the half-pixel offset, closed shapes are stroked inside, `ImDrawListFlags` merged into `ImDrawFlags`, `ImDrawFlags_RoundCorners*` renamed | compile error + lines move | 47c994a, 7eeb916 |
| 1.93: `ImGuiTextFilter::Draw()` lost its `width` parameter | compile error | 09e2ba7 |

Verified (2026-10-23): switching implot to `v1.0` and forcing a build with `-DYC_ALLOW_DEP_DRIFT=ON` gives these errors, confirming the table:

```
implot_items.cpp(161): error C2065: 'ImDrawListFlags_AntiAliasedLines': undeclared identifier
implot_items.cpp(162): error C2065: 'ImDrawListFlags_AntiAliasedLinesUseTex': undeclared identifier
implot.cpp(3175):      error C2065: 'ImDrawFlags_RoundCornersAll': undeclared identifier
implot.cpp(5082):      error C2660: 'ImGuiTextFilter::Draw': function does not take 2 arguments
yc_imgui_check.cpp(20): error C2338: static assertion failed: 'the implot version doesn't match third_party/deps.lock'
```

Our own code must follow the post-1.92.8 signature too: `AddRect(p_min, p_max, col, rounding, thickness, flags)`.

## Three locks

1. **`deps.lock`**: records the URL, tag, commit and version numbers of every submodule.
2. **CMake configure step** ([`cmake/YcDeps.cmake`](../cmake/YcDeps.cmake)): an uninitialized submodule, a commit mismatch, an `IMGUI_VERSION_NUM` / `IMPLOT_VERSION_NUM` mismatch, a non-docking branch or a `vcpkg.json` baseline mismatch each cause `FATAL_ERROR`.
3. **Compile time** ([`src/ui/yc_imgui_check.cpp`](../src/ui/yc_imgui_check.cpp)): `static_assert` on the version numbers, `IMGUI_HAS_DOCK`, 32-bit `ImDrawIdx` and `ImWchar`.

Custom settings always go into [`src/ui/yc_imconfig.h`](../src/ui/yc_imconfig.h) (injected into every TU through `IMGUI_USER_CONFIG`); **never modify submodule contents**.

## Getting the sources for the first time

```cmd
git clone --recurse-submodules https://github.com/ycgrid/ycgrid.git
rem already cloned:
git submodule update --init --recursive
scripts\build.cmd
```

## Upgrade procedure (ImGui / ImPlot)

1. Create a branch and write an ADR (`docs/adr/NNNN-upgrade-imgui-x.y.md`).
2. First check whether ImPlot supports the new ImGui: **when ImPlot lags behind, don't upgrade ImGui yet**.
3. Move the submodules:
   ```cmd
   git -C third_party\imgui fetch --tags
   git -C third_party\imgui checkout v1.xx.x-docking
   git -C third_party\implot fetch
   git -C third_party\implot checkout <commit>
   ```
4. Update `ref`, `commit`, `version` and `version_num` in `deps.lock`.
5. `scripts\build.cmd`: must build with zero warnings under `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`.
6. Smoke test: `ycgrid.exe --frames 300 --report r.json --screenshot s.png`, and check the ImPlot / ImGui demos by eye.
7. Screenshot comparison and no regression in the performance benchmarks (PLAN §17).
8. Commit `git add third_party/imgui third_party/implot third_party/deps.lock` together.
