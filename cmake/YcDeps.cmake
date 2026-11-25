# SPDX-License-Identifier: Apache-2.0
# YcDeps.cmake - version checks of the third-party submodules (configure step)
#
# The first of three locks (the other two: deps.lock itself, the static_asserts in src/ui/yc_imgui_check.cpp):
#   1. submodules must be initialized
#   2. IMGUI_VERSION_NUM and IMGUI_HAS_DOCK in imgui.h must match deps.lock
#   3. IMPLOT_VERSION_NUM in implot.h must match deps.lock
#   4. the current commit of each submodule must equal the commit recorded in deps.lock
# Any mismatch is a FATAL_ERROR; -DYC_ALLOW_DEP_DRIFT=ON turns it into a warning (for upgrade tests only).

set(YC_DEPS_LOCK "${PROJECT_SOURCE_DIR}/third_party/deps.lock")

function(yc_read_lock key out_var)
  file(STRINGS "${YC_DEPS_LOCK}" _line REGEX "^${key}=")
  if(NOT _line)
    message(FATAL_ERROR "deps.lock is missing the field ${key}")
  endif()
  string(REGEX REPLACE "^${key}=" "" _value "${_line}")
  set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(yc_dep_problem msg)
  if(YC_ALLOW_DEP_DRIFT)
    message(WARNING "[YC_ALLOW_DEP_DRIFT] ${msg}")
  else()
    message(FATAL_ERROR "${msg}\n(for upgrade tests add -DYC_ALLOW_DEP_DRIFT=ON; for a real upgrade update deps.lock as described in third_party/README.md)")
  endif()
endfunction()

function(yc_check_submodule name header)
  yc_read_lock("${name}.path" _path)
  yc_read_lock("${name}.commit" _commit)
  set(_dir "${PROJECT_SOURCE_DIR}/${_path}")
  if(NOT EXISTS "${_dir}/${header}")
    message(FATAL_ERROR
      "${_path}/${header} not found: the submodule is not initialized.\n"
      "Run in the repo root: git submodule update --init --recursive")
  endif()
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${_dir}" rev-parse HEAD
      OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rc ERROR_QUIET)
    if(_rc EQUAL 0 AND NOT _head STREQUAL _commit)
      yc_dep_problem("${name} is at commit ${_head}, but deps.lock requires ${_commit}.\n"
                     "To restore: git -C ${_path} checkout ${_commit}")
    endif()
  endif()
  set(YC_${name}_DIR "${_dir}" PARENT_SCOPE)
endfunction()

yc_check_submodule(imgui imgui.h)
yc_check_submodule(implot implot.h)
set(YC_IMGUI_DIR "${YC_imgui_DIR}")
set(YC_IMPLOT_DIR "${YC_implot_DIR}")

# --- imgui version ---
yc_read_lock(imgui.version_num YC_EXPECTED_IMGUI_VERSION_NUM)
file(STRINGS "${YC_IMGUI_DIR}/imgui.h" _imgui_ver REGEX "^#define IMGUI_VERSION_NUM[ \t]+[0-9]+")
string(REGEX MATCH "[0-9]+$" _imgui_ver "${_imgui_ver}")
if(NOT _imgui_ver STREQUAL YC_EXPECTED_IMGUI_VERSION_NUM)
  yc_dep_problem("imgui IMGUI_VERSION_NUM=${_imgui_ver}, deps.lock requires ${YC_EXPECTED_IMGUI_VERSION_NUM}")
endif()
file(STRINGS "${YC_IMGUI_DIR}/imgui.h" _imgui_dock REGEX "^#define IMGUI_HAS_DOCK")
if(NOT _imgui_dock)
  yc_dep_problem("third_party/imgui is not the docking branch (imgui.h has no IMGUI_HAS_DOCK)")
endif()

# --- implot version ---
yc_read_lock(implot.version_num YC_EXPECTED_IMPLOT_VERSION_NUM)
file(STRINGS "${YC_IMPLOT_DIR}/implot.h" _implot_ver REGEX "^#define IMPLOT_VERSION_NUM[ \t]+[0-9]+")
string(REGEX MATCH "[0-9]+$" _implot_ver "${_implot_ver}")
if(NOT _implot_ver STREQUAL YC_EXPECTED_IMPLOT_VERSION_NUM)
  yc_dep_problem("implot IMPLOT_VERSION_NUM=${_implot_ver}, deps.lock requires ${YC_EXPECTED_IMPLOT_VERSION_NUM}")
endif()

# --- vcpkg baseline consistency ---
yc_read_lock(vcpkg.baseline _vcpkg_baseline)
file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" _vcpkg_json)
string(FIND "${_vcpkg_json}" "${_vcpkg_baseline}" _pos)
if(_pos EQUAL -1)
  yc_dep_problem("builtin-baseline in vcpkg.json doesn't match vcpkg.baseline in deps.lock")
endif()

message(STATUS "YCGrid deps: imgui ${_imgui_ver} (docking), implot ${_implot_ver}, both match deps.lock")
