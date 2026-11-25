# SPDX-License-Identifier: Apache-2.0
# YcStageDist.cmake - after building ycgrid, lay out a "double-click to run" folder (dist_debug / dist_release)
#
# Called by the POST_BUILD step of apps/ycgrid:
#   cmake -DSRC=<repo> -DDEST=<dist folder> -DEXE=<ycgrid.exe> -DPDB=<ycgrid.pdb> -P YcStageDist.cmake
#
# Contents: ycgrid.exe (+ pdb), python\yc (with the MCP tool definitions), skill, mcp, install scripts, licenses,
#           the portable Python (copied from build\runtime; run scripts\setup_runtime.cmd when it is missing), and the
#           examples (examples\: the demo, the trading screen and the research and trading screens, with their lib).
# file(COPY) skips files with the same timestamp, so repeated builds are fast. python\yc, skill, mcp and licenses are replaced completely every time (renamed or deleted files don't linger).
# Examples: missing files are added; files are updated when the repo version is newer (files you changed that are newer are kept).
cmake_minimum_required(VERSION 3.28)

foreach(v SRC DEST EXE)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "YcStageDist: missing -D${v}")
  endif()
endforeach()

file(MAKE_DIRECTORY "${DEST}")
file(COPY "${EXE}" DESTINATION "${DEST}")
if(DEFINED PDB AND EXISTS "${PDB}")
  file(COPY "${PDB}" DESTINATION "${DEST}")
endif()

# Python interface, the skill for AI agents, the MCP tool list
file(REMOVE_RECURSE "${DEST}/python/yc" "${DEST}/skill" "${DEST}/mcp")
file(COPY "${SRC}/python/yc" DESTINATION "${DEST}/python" PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
file(COPY "${SRC}/mcp/tools.json" DESTINATION "${DEST}/python/yc/mcp")
file(COPY "${SRC}/skill" DESTINATION "${DEST}")
file(COPY "${SRC}/mcp" DESTINATION "${DEST}" PATTERN "__pycache__" EXCLUDE)

# installation and documentation
foreach(f install.cmd uninstall.cmd README.txt)
  if(EXISTS "${SRC}/packaging/${f}")
    file(COPY "${SRC}/packaging/${f}" DESTINATION "${DEST}")
  endif()
endforeach()
# emptied first: a license left by an older build (a component since removed) must not ship in a release or the wheel
file(REMOVE_RECURSE "${DEST}/licenses")
file(MAKE_DIRECTORY "${DEST}/licenses")
set(vcpkg_share "build/vcpkg_installed/x64-windows-static/share")
foreach(pair "LICENSE|ycgrid-LICENSE.txt" "NOTICE|NOTICE.txt" "THIRD-PARTY-NOTICES.md|THIRD-PARTY-NOTICES.md"
             "third_party/imgui/LICENSE.txt|imgui-LICENSE.txt" "third_party/implot/LICENSE|implot-LICENSE.txt"
             "third_party/fluent-icons/LICENSE|fluent-icons-LICENSE.txt" "third_party/uv-LICENSE-MIT.txt|uv-LICENSE.txt"
             "${vcpkg_share}/freetype/copyright|freetype-LICENSE.txt" "${vcpkg_share}/pugixml/copyright|pugixml-LICENSE.txt"
             "${vcpkg_share}/nlohmann-json/copyright|nlohmann-json-LICENSE.txt" "${vcpkg_share}/zlib/copyright|zlib-LICENSE.txt"
             "${vcpkg_share}/zstd/copyright|zstd-LICENSE.txt")
  string(REPLACE "|" ";" p "${pair}")
  list(GET p 0 from)
  list(GET p 1 to)
  if(EXISTS "${SRC}/${from}")
    configure_file("${SRC}/${from}" "${DEST}/licenses/${to}" COPYONLY)
  endif()
endforeach()

# portable Python: build\runtime\python + uv.exe (without caches and temporary files)
set(rt "${SRC}/build/runtime")
if(EXISTS "${rt}/python/python.exe")
  file(COPY "${rt}/python" DESTINATION "${DEST}/runtime" PATTERN "__pycache__" EXCLUDE)
  foreach(f uv.exe uvw.exe uvx.exe)
    if(EXISTS "${rt}/${f}")
      file(COPY "${rt}/${f}" DESTINATION "${DEST}/runtime")
    endif()
  endforeach()
else()
  message(WARNING "YCGrid: build\\runtime\\python not found; ${DEST} has no Python runtime. Run scripts\\setup_runtime.cmd first, then build again.")
endif()

# [YC] the examples, every one a project folder (missing files are added; files are updated when the repo version is
# newer): demo, txo_market_maker, the research and trading screens, and lib, the code they share
set(ex_src "${SRC}/examples")
set(ex_dst "${DEST}/examples")
if(EXISTS "${ex_src}")
  file(GLOB_RECURSE ex_files RELATIVE "${ex_src}" "${ex_src}/*")
  foreach(f IN LISTS ex_files)
    if(f MATCHES "__pycache__" OR f MATCHES "(^|/)\\.ycgrid/")
      continue()
    endif()
    if(NOT EXISTS "${ex_dst}/${f}" OR "${ex_src}/${f}" IS_NEWER_THAN "${ex_dst}/${f}")
      get_filename_component(d "${ex_dst}/${f}" DIRECTORY)
      file(MAKE_DIRECTORY "${d}")
      file(COPY "${ex_src}/${f}" DESTINATION "${d}")
    endif()
  endforeach()
  # MCP settings (used by AI agents in the demo project): point at this dist's Python and yc package
  file(TO_NATIVE_PATH "${DEST}/runtime/python/python.exe" py_native)
  file(TO_NATIVE_PATH "${DEST}/python" yc_native)
  string(REPLACE "\\" "\\\\" py_json "${py_native}")
  string(REPLACE "\\" "\\\\" yc_json "${yc_native}")
  set(boot "import sys;sys.path.insert(0,r'${yc_json}');from yc.mcp.server import main;sys.exit(main())")
  file(WRITE "${ex_dst}/demo/.mcp.json"
       "{\n  \"mcpServers\": {\n    \"ycgrid\": {\n      \"command\": \"${py_json}\",\n      \"args\": [\"-E\", \"-s\", \"-c\", \"${boot}\"]\n    }\n  }\n}\n")
endif()

# open the demo in one step
file(WRITE "${DEST}/open_demo.cmd"
     "@echo off\r\nchcp 65001 >nul\r\nrem Opens the demo project with this folder's ycgrid.exe (the Python code is in examples\\demo\\main.py)\r\nstart \"\" \"%~dp0ycgrid.exe\" \"%~dp0examples\\demo\\demo.ycg\"\r\n")

message(STATUS "YCGrid: staged ${DEST}")
