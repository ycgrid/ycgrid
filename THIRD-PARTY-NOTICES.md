# Third-party notices

YCGrid is licensed under the [Apache License 2.0](LICENSE). It is built with, and a release ships, the third-party
components below, each under its own license. In a release the full license texts are in the `licenses\` folder; the
file names are given in the last column. Versions are pinned in [third_party/deps.lock](third_party/deps.lock) and
[vcpkg.json](vcpkg.json).

## Linked into ycgrid.exe

| Component | Version | License | Copyright | License file in a release |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) | 1.93.0 | MIT | (c) 2014-2026 Omar Cornut | `imgui-LICENSE.txt` |
| [ImPlot](https://github.com/epezent/implot) | 1.1 WIP (`851cdbd`) | MIT | (c) 2020 Evan Pezent | `implot-LICENSE.txt` |
| [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons) (icon font, embedded) | `463d58b` | MIT | (c) 2020 Microsoft Corporation | `fluent-icons-LICENSE.txt` |
| [FreeType](https://freetype.org) | 2.14.3 | FreeType License (FTL), chosen from FTL or GPL-2.0-or-later | (c) 1996-2026 The FreeType Project | `freetype-LICENSE.txt` |
| [pugixml](https://pugixml.org) | 1.16 | MIT | (c) 2006-2026 Arseny Kapoulkine | `pugixml-LICENSE.txt` |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.12.0 | MIT | (c) 2013-2025 Niels Lohmann | `nlohmann-json-LICENSE.txt` |
| [zlib](https://zlib.net) | 1.3.2 | zlib License | (c) 1995-2026 Jean-loup Gailly and Mark Adler | `zlib-LICENSE.txt` |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | BSD-3-Clause, chosen from BSD-3-Clause or GPL-2.0-only | (c) Meta Platforms, Inc. and affiliates | `zstd-LICENSE.txt` |
| [Natural Earth](https://www.naturalearthdata.com) (map chart shapes, generated into `src/engine/geo_data.inc`) | 5.x | Public domain | none | none needed |

FreeType credit, as its license requires: Portions of this software are copyright (c) 2026 The FreeType Project
(www.freetype.org). All rights reserved.

Map shapes: Made with Natural Earth.

Other fonts: the Latin, CJK and symbol fonts are read at run time from the fonts installed in Windows. YCGrid doesn't
ship any of them.

## Shipped next to ycgrid.exe

| Component | Version | License | Copyright | License file in a release |
|---|---|---|---|---|
| [uv](https://github.com/astral-sh/uv) (`runtime\uv.exe`) | 0.12.24 | MIT or Apache-2.0 (MIT chosen) | (c) 2025 Astral Software Inc. | `uv-LICENSE.txt` |
| [CPython](https://www.python.org) (`runtime\python`) | 3.12.15 | PSF License, with the licenses of the libraries built into it (OpenSSL, SQLite, libffi, bzip2, xz, zlib, Tcl/Tk, ...) | (c) 2001-2026 Python Software Foundation | `runtime\python\LICENSE.txt` (Tcl/Tk also `runtime\python	cl	k8.6\license.terms`) |
| [python-build-standalone](https://github.com/astral-sh/python-build-standalone) (the build of CPython above, installed by uv) | as CPython | its build system is MPL-2.0; the binaries are CPython's | Astral Software Inc. and contributors | source: the project's repository |
| Microsoft Visual C++ runtime (`runtime\python\vcruntime140.dll`, `vcruntime140_1.dll`) | 14.x | Microsoft Distributable Code, redistributed with the Windows build of CPython on the terms in its license | (c) Microsoft Corporation | `runtime\python\LICENSE.txt` ("Additional Conditions for this Windows binary build") |
| [pip](https://pip.pypa.io) (bundled with CPython, `runtime\python\Lib\site-packages\pip`) | as installed | MIT | (c) The pip developers | `pip-*.dist-info` |

Python packages are not part of a release. numpy, pandas and the other packages in
[python/requirements-runtime.txt](python/requirements-runtime.txt), and those a project lists in its `requirements.txt`,
are downloaded from PyPI onto the user's computer when the runtime is set up or a project asks for them; each comes with
its own license in its `*.dist-info` folder (numpy, for example, includes OpenBLAS and LAPACK, BSD-3-Clause).

## Used only to build and test (not shipped)

| Component | License |
|---|---|
| [Catch2](https://github.com/catchorg/Catch2) | BSL-1.0 |
| [vcpkg](https://github.com/microsoft/vcpkg), CMake, MSVC | their own licenses |

## Trademarks

Microsoft, Excel and Office are trademarks of the Microsoft group of companies. YCGrid is an independent project; it is
not affiliated with, endorsed by or sponsored by Microsoft. Excel's function names, object model names and file format
names are used only to describe compatibility. No Microsoft Office icons, images or other proprietary assets are used;
the icons come from the open-source Fluent UI System Icons set listed above.

Python is a trademark of the Python Software Foundation. Other names and marks are the property of their owners.
Company names and stock codes in the examples (TSMC, Fubon, Cathay...) are sample data; their prices are simulated.
