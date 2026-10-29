# Vendor / presets / hotkeys tests.
#
# vendor_test: headless libobs test (installed OBS's obs.dll, no UI, no
#   obs-websocket). Compiles src/vendor, src/presets and the core into one
#   executable and drives every vendor request through stage_vendor_dispatch,
#   the events pump with a fake clock, and a threaded stress run.
#   Only in the plugin build (needs the OBS headers and import libraries) and
#   only when an OBS install is found.
#
# smoke.mjs: live check against a running OBS with obs-websocket (manual:
#   node tests/vendor-smoke/smoke.mjs [ws://127.0.0.1:4455] [password]).
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT TARGET parallaxa)
  return()
endif()

# Installed OBS Studio: -DSTAGE_OBS_DIR, else the OBS_DIR environment
# variable, else the default installer folder (tools/build.ps1 passes the
# folder release/obs-detect.ps1 finds).
if(DEFINED ENV{OBS_DIR})
  file(TO_CMAKE_PATH "$ENV{OBS_DIR}" _stage_obs_default)
else()
  set(_stage_obs_default "C:/Program Files/obs-studio")
endif()
set(STAGE_OBS_DIR "${_stage_obs_default}"
    CACHE PATH "Installed OBS Studio (used by the headless tests)")
if(NOT EXISTS "${STAGE_OBS_DIR}/bin/64bit/obs.dll")
  message(STATUS "vendor_test skipped: no OBS at ${STAGE_OBS_DIR}")
  return()
endif()

set(_plugin "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(GLOB _vendor_sources CONFIGURE_DEPENDS
  "${_plugin}/src/vendor/*.c"
  "${_plugin}/src/presets/*.c")

add_executable(vendor_test
  vendor_test.c
  ${_vendor_sources}
  "${_plugin}/src/core/stage-core.c"
  "${_plugin}/src/core/stage-cmdq.c"
  "${_plugin}/src/core/stage-clock.c"
  "${_plugin}/src/core/stage-frontend.c")
target_compile_definitions(vendor_test PRIVATE
  STAGE_VERSION="${parallaxa_VERSION}" STAGE_HAS_VENDOR=1 STAGE_HAS_PRESETS=1)
target_include_directories(vendor_test PRIVATE
  "${_plugin}/include" "${_plugin}/src" "${_plugin}/src/core")
target_include_directories(vendor_test SYSTEM PRIVATE
  "${CMAKE_BINARY_DIR}/generated"
  "${STAGE_OBS_SRC}/libobs"
  "${STAGE_OBS_SRC}/frontend/api"
  "${STAGE_OBS_SRC}/deps/w32-pthreads"
  "${_plugin}/third_party")
target_link_libraries(vendor_test PRIVATE
  "${STAGE_IMPLIB_DIR}/obs.lib"
  "${STAGE_IMPLIB_DIR}/obs-frontend-api.lib"
  "${STAGE_IMPLIB_DIR}/w32-pthreads.lib")
if(TARGET stage_math)
  target_compile_definitions(vendor_test PRIVATE STAGE_HAS_MATH=1)
  target_link_libraries(vendor_test PRIVATE stage_math)
endif()
if(MSVC)
  target_compile_options(vendor_test PRIVATE /W4 /WX /utf-8)
endif()

set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/vendor_test_user")
# Fresh user-config folder for every run (fixture).
add_test(NAME vendor_test_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_scratch}")
set_tests_properties(vendor_test_clean PROPERTIES FIXTURES_SETUP vendor_scratch)
add_test(NAME vendor_test
  COMMAND vendor_test "${_plugin}/data" "${_scratch}" "${CMAKE_CURRENT_BINARY_DIR}/vendor_payloads.jsonl")
set_tests_properties(vendor_test PROPERTIES
  FIXTURES_REQUIRED vendor_scratch
  FIXTURES_SETUP vendor_payloads
  TIMEOUT 120
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${STAGE_OBS_DIR}/bin/64bit")

# Every response / event payload vendor_test produced, validated against the
# normative JSON Schemas (docs/protocol/schemas) by smoke.mjs --validate.
find_program(STAGE_NODE node)
if(STAGE_NODE)
  add_test(NAME vendor_schema_check
    COMMAND "${STAGE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/smoke.mjs"
            --validate "${CMAKE_CURRENT_BINARY_DIR}/vendor_payloads.jsonl")
  set_tests_properties(vendor_schema_check PROPERTIES FIXTURES_REQUIRED vendor_payloads TIMEOUT 60)
endif()
