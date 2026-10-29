# Core unit tests (command queue, registries, snapshots). No libobs: the core
# sources are compiled with STAGE_CORE_NO_OBS and, on Windows, a pthread shim.
# Works from the plugin build (add_subdirectory) or standalone:
#   cmake -S tests/core -B build-core-test -G Ninja && cmake --build build-core-test
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(stage_core_tests LANGUAGES C)
  set(CMAKE_C_STANDARD 17)
  set(CMAKE_C_STANDARD_REQUIRED ON)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  enable_testing()
endif()

set(_plugin "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(_core "${_plugin}/src/core")

add_executable(core_cmdq_test
  core_cmdq_test.c
  "${_core}/stage-cmdq.c"
  "${_core}/stage-core.c"
  "${_core}/stage-clock.c")
target_compile_definitions(core_cmdq_test PRIVATE STAGE_CORE_NO_OBS=1)
target_include_directories(core_cmdq_test PRIVATE "${_core}" "${_plugin}/include")
if(WIN32)
  # Must come first so <pthread.h> resolves to the shim.
  target_include_directories(core_cmdq_test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim")
else()
  find_package(Threads REQUIRED)
  target_link_libraries(core_cmdq_test PRIVATE Threads::Threads)
endif()
if(MSVC)
  target_compile_options(core_cmdq_test PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(core_cmdq_test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME core_cmdq_test COMMAND core_cmdq_test)
set_tests_properties(core_cmdq_test PROPERTIES TIMEOUT 120)

# Locale files: identical key sets, Key="Value" syntax, no BOM.
add_test(NAME core_locale_check
  COMMAND "${CMAKE_COMMAND}" "-DLOCALE_DIR=${_plugin}/data/locale" -P "${CMAKE_CURRENT_SOURCE_DIR}/locale_check.cmake")

# Headless libobs load test: loads parallaxa.dll with the installed OBS's
# obs.dll (no OBS UI, no graphics, nothing written) and checks the load log
# line, locale, post_load/unload and the allocation count after shutdown.
# Only in the plugin build, and only when an OBS install is found.
# Installed OBS Studio: -DSTAGE_OBS_DIR, else the OBS_DIR environment
# variable, else the default installer folder (tools/build.ps1 passes the
# folder release/obs-detect.ps1 finds).
if(DEFINED ENV{OBS_DIR})
  file(TO_CMAKE_PATH "$ENV{OBS_DIR}" _stage_obs_default)
else()
  set(_stage_obs_default "C:/Program Files/obs-studio")
endif()
set(STAGE_OBS_DIR "${_stage_obs_default}"
    CACHE PATH "Installed OBS Studio (used by the core_obs_load_smoke test)")
if(TARGET parallaxa AND EXISTS "${STAGE_OBS_DIR}/bin/64bit/obs.dll")
  add_executable(obs_load_smoke obs_load_smoke.c)
  target_compile_definitions(obs_load_smoke PRIVATE STAGE_VERSION="${parallaxa_VERSION}")
  target_include_directories(obs_load_smoke PRIVATE "${_plugin}/include")
  target_include_directories(obs_load_smoke SYSTEM PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    "${STAGE_OBS_SRC}/libobs"
    "${STAGE_OBS_SRC}/deps/w32-pthreads")
  target_link_libraries(obs_load_smoke PRIVATE "${STAGE_IMPLIB_DIR}/obs.lib")
  if(MSVC)
    target_compile_options(obs_load_smoke PRIVATE /W4 /WX /utf-8)
  endif()
  add_dependencies(obs_load_smoke parallaxa)
  add_test(NAME core_obs_load_smoke
    COMMAND obs_load_smoke "$<TARGET_FILE:parallaxa>" "${_plugin}/data")
  set_tests_properties(core_obs_load_smoke PROPERTIES
    TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${STAGE_OBS_DIR}/bin/64bit")
endif()
