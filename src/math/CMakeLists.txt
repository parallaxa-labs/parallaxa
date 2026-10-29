# stage_math — pure C17 static library (no libobs headers, no implibs).
# Builds standalone:
#   cmake -S src/math -B build-math -G Ninja && cmake --build build-math
# or as part of the plugin (CMakeLists.txt: add_subdirectory(src/math)).
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(stage_math LANGUAGES C)
endif()

# Guard against being added twice (plugin build + tests/ standalone path).
if(TARGET stage_math)
  return()
endif()

file(GLOB STAGE_MATH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

add_library(stage_math STATIC ${STAGE_MATH_SOURCES})
target_include_directories(stage_math PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set_target_properties(stage_math PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # /utf-8: sources and stage-protocol.h contain UTF-8 text; without it a
  # CP932 (Japanese) system code page raises C4819, which /WX makes fatal.
  target_compile_options(stage_math PRIVATE /utf-8 /W4 /WX)
else()
  target_compile_options(stage_math PRIVATE -Wall -Wextra)
  target_link_libraries(stage_math PUBLIC m)
endif()
