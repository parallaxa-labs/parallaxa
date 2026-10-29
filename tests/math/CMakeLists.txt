# stage_math unit tests.
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT TARGET stage_math)
  message(STATUS "stage_math not present: skipping math tests")
  return()
endif()

add_executable(math_test math_test.c)
target_link_libraries(math_test PRIVATE stage_math)
set_target_properties(math_test PROPERTIES
  C_STANDARD 17
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
if(MSVC)
  # /utf-8: see src/math/CMakeLists.txt (C4819 under CP932 with /WX).
  target_compile_options(math_test PRIVATE /utf-8 /W4 /WX)
else()
  target_compile_options(math_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME stage_math COMMAND math_test)

# Acceptance: no libobs / OBS frontend header is included from src/math.
add_test(NAME stage_math_no_obs_includes
  COMMAND "${CMAKE_COMMAND}"
    "-DMATH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../../src/math"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check-no-obs.cmake")
