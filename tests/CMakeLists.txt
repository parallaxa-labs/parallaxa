# Parallaxa plugin unit tests.
#
# Every subdirectory with its own CMakeLists.txt is added (math/, core/,
# camera/, fx/, vendor-smoke/, and contracts/ = protocol and preset checks
# with Node.js). Subdirectories may rely only on the
# include/ directory and the `stage_math` target; anything else they must
# guard themselves (STAGE_TESTS_STANDALONE is ON when this directory is the
# top-level project, i.e. no plugin/OBS targets exist).
#
# Standalone (no OBS headers or import libraries needed):
#   cmake -S tests -B build-tests -G Ninja
#   cmake --build build-tests && ctest --test-dir build-tests
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(stage_tests LANGUAGES C)
  set(STAGE_TESTS_STANDALONE ON)
else()
  set(STAGE_TESTS_STANDALONE OFF)
endif()

# Harmless when the parent already called it; the parent must call
# enable_testing() itself for `ctest --test-dir <plugin build>` to see tests.
enable_testing()

# The plugin build adds src/math before tests/; standalone we add it here.
set(_stage_math_dir "${CMAKE_CURRENT_SOURCE_DIR}/../src/math")
if(NOT TARGET stage_math AND EXISTS "${_stage_math_dir}/CMakeLists.txt")
  add_subdirectory("${_stage_math_dir}" "${CMAKE_CURRENT_BINARY_DIR}/stage_math")
endif()

file(GLOB _stage_test_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(SORT _stage_test_dirs)
foreach(_dir IN LISTS _stage_test_dirs)
  if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
    add_subdirectory("${_dir}")
  endif()
endforeach()
