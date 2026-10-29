# Parallaxa — OBS Studio plugin (parallaxa.dll)
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds against the libobs headers (third_party/obs-src, fetched by
# tools/fetch-obs-headers.ps1) and import libs generated from an installed
# OBS (third_party/implibs, tools/make-implibs.ps1). No Qt, no libobs
# build. Typical use: powershell -File tools/build.ps1
#
# Modules: every src/<mod>/ directory (core, camera, fx, vendor, presets) that
# contains .c files is compiled in and defines STAGE_HAS_<MOD>=1; absent
# modules are simply skipped. src/math is a static library (stage_math) with
# its own CMakeLists.txt when present.

cmake_minimum_required(VERSION 3.25)

project(parallaxa VERSION 0.1.0 DESCRIPTION "Parallaxa OBS plugin" LANGUAGES C)

if(WIN32)
  enable_language(RC)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(STAGE_OBS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-src"
    CACHE PATH "obs-studio source tree (only libobs, frontend/api and deps/w32-pthreads are used)")
set(STAGE_IMPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/implibs"
    CACHE PATH "Folder with obs.lib, obs-frontend-api.lib and w32-pthreads.lib")
set(STAGE_PROJECT_URL "https://github.com/parallaxa-labs/parallaxa"
    CACHE STRING "Public repository URL (About row docs / bug links)")
if(NOT STAGE_PROJECT_URL MATCHES "^https://[^\"\\\\ ]+$")
  message(FATAL_ERROR "STAGE_PROJECT_URL must be an https:// URL without quotes, backslashes or spaces")
endif()
option(STAGE_BUILD_PLUGIN "Build parallaxa.dll (needs STAGE_OBS_SRC and STAGE_IMPLIB_DIR)" ON)
option(STAGE_BUILD_TESTS "Build the unit tests (no libobs needed)" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# OBS ships the release VC runtime; always link the DLL CRT (/MD).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase") # /Zi -> separate PDB

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  # Full inlining in optimized builds (CMake's RelWithDebInfo default is /Ob1).
  string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  set(STAGE_C_OPTIONS /W4 /utf-8 /Zc:preprocessor $<$<NOT:$<CONFIG:Debug>>:/O2 /Gy /Gw>)
  set(STAGE_LINK_OPTIONS /DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO)
  # MSVC's <time.h> defines struct timespec; tell w32-pthreads not to.
  add_compile_definitions(HAVE_STRUCT_TIMESPEC _CRT_SECURE_NO_WARNINGS)
else()
  set(STAGE_C_OPTIONS -Wall -Wextra $<$<NOT:$<CONFIG:Debug>>:-O2>)
  set(STAGE_LINK_OPTIONS)
endif()

set(STAGE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(STAGE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ---------------------------------------------------------------------------
# Optional modules
# ---------------------------------------------------------------------------
set(STAGE_MODULES)
set(STAGE_SOURCES)
foreach(mod IN ITEMS core camera fx vendor presets)
  if(IS_DIRECTORY "${STAGE_SRC_DIR}/${mod}")
    file(GLOB _mod_sources CONFIGURE_DEPENDS "${STAGE_SRC_DIR}/${mod}/*.c")
    if(_mod_sources)
      list(APPEND STAGE_SOURCES ${_mod_sources})
      list(APPEND STAGE_MODULES ${mod})
      string(TOUPPER "${mod}" _MOD)
      add_compile_definitions(STAGE_HAS_${_MOD}=1)
    endif()
  endif()
endforeach()
if(NOT "core" IN_LIST STAGE_MODULES)
  message(FATAL_ERROR "src/core is required")
endif()

# src/math: pure C static library, buildable without OBS.
if(EXISTS "${STAGE_SRC_DIR}/math/CMakeLists.txt")
  add_subdirectory(src/math)
elseif(IS_DIRECTORY "${STAGE_SRC_DIR}/math")
  file(GLOB _math_sources CONFIGURE_DEPENDS "${STAGE_SRC_DIR}/math/*.c")
  if(_math_sources)
    add_library(stage_math STATIC ${_math_sources})
    target_include_directories(stage_math PUBLIC "${STAGE_SRC_DIR}/math" "${STAGE_INCLUDE_DIR}")
    target_compile_options(stage_math PRIVATE ${STAGE_C_OPTIONS})
  endif()
endif()
if(TARGET stage_math)
  list(APPEND STAGE_MODULES math)
endif()
message(STATUS "Parallaxa ${PROJECT_VERSION}: modules = ${STAGE_MODULES}")

# ---------------------------------------------------------------------------
# Plugin DLL
# ---------------------------------------------------------------------------
if(STAGE_BUILD_PLUGIN)
  foreach(_hdr IN ITEMS libobs/obs-module.h frontend/api/obs-frontend-api.h deps/w32-pthreads/pthread.h)
    if(NOT EXISTS "${STAGE_OBS_SRC}/${_hdr}")
      message(FATAL_ERROR "Missing ${STAGE_OBS_SRC}/${_hdr}. Run tools/fetch-obs-headers.ps1 "
                          "or set -DSTAGE_OBS_SRC=<obs-studio checkout>.")
    endif()
  endforeach()
  foreach(_lib IN ITEMS obs obs-frontend-api w32-pthreads)
    if(NOT EXISTS "${STAGE_IMPLIB_DIR}/${_lib}.lib")
      message(FATAL_ERROR "Missing ${STAGE_IMPLIB_DIR}/${_lib}.lib. Run tools/make-implibs.ps1 "
                          "or set -DSTAGE_IMPLIB_DIR=<folder>.")
    endif()
  endforeach()

  # libobs/obsconfig.h is generated by libobs' own configure step; provide a
  # stub when building against a plain source checkout.
  set(_obsconfig_dirs)
  if(NOT EXISTS "${STAGE_OBS_SRC}/libobs/obsconfig.h")
    configure_file(cmake/obsconfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h" COPYONLY)
    list(APPEND _obsconfig_dirs "${CMAKE_CURRENT_BINARY_DIR}/generated")
  endif()

  set(_rc)
  if(WIN32)
    configure_file(cmake/stage-version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/stage-version.rc" @ONLY)
    set(_rc "${CMAKE_CURRENT_BINARY_DIR}/generated/stage-version.rc")
  endif()

  add_library(parallaxa MODULE ${STAGE_SOURCES} ${_rc})
  set_target_properties(parallaxa PROPERTIES PREFIX "" OUTPUT_NAME "parallaxa")
  target_compile_definitions(parallaxa PRIVATE STAGE_VERSION="${PROJECT_VERSION}" STAGE_PROJECT_URL="${STAGE_PROJECT_URL}")
  target_compile_options(parallaxa PRIVATE ${STAGE_C_OPTIONS})
  target_link_options(parallaxa PRIVATE ${STAGE_LINK_OPTIONS})
  target_include_directories(parallaxa PRIVATE
    "${STAGE_INCLUDE_DIR}" "${STAGE_SRC_DIR}" "${STAGE_SRC_DIR}/core")
  # Third-party headers as SYSTEM (/external:I, warnings off) so /W4 only
  # reports our own code.
  target_include_directories(parallaxa SYSTEM PRIVATE
    ${_obsconfig_dirs}
    "${STAGE_OBS_SRC}/libobs"
    "${STAGE_OBS_SRC}/frontend/api"
    "${STAGE_OBS_SRC}/deps/w32-pthreads"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_link_libraries(parallaxa PRIVATE
    "${STAGE_IMPLIB_DIR}/obs.lib"
    "${STAGE_IMPLIB_DIR}/obs-frontend-api.lib"
    "${STAGE_IMPLIB_DIR}/w32-pthreads.lib")
  if(TARGET stage_math)
    target_compile_definitions(parallaxa PRIVATE STAGE_HAS_MATH=1)
    target_link_libraries(parallaxa PRIVATE stage_math)
    target_include_directories(parallaxa PRIVATE "${STAGE_SRC_DIR}/math")
  endif()

  # `cmake --install` produces a ready-to-copy plugin folder with BOTH
  # layouts sharing one data folder: OBS 33+ loads the flat dll, OBS 32.x
  # the legacy bin/64bit copy (Steam updates OBS silently, so a plugin
  # installed for 32.x must keep loading after the update to 33).
  set(_dest "parallaxa")
  install(TARGETS parallaxa LIBRARY DESTINATION "${_dest}" RUNTIME DESTINATION "${_dest}")
  install(TARGETS parallaxa LIBRARY DESTINATION "${_dest}/bin/64bit" RUNTIME DESTINATION "${_dest}/bin/64bit")
  install(FILES $<TARGET_PDB_FILE:parallaxa> DESTINATION "${_dest}" OPTIONAL)
  install(FILES $<TARGET_PDB_FILE:parallaxa> DESTINATION "${_dest}/bin/64bit" OPTIONAL)
  install(DIRECTORY data/ DESTINATION "${_dest}/data")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(STAGE_BUILD_TESTS)
  enable_testing()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core/CMakeLists.txt")
    add_subdirectory(tests/core)
  endif()
endif()
