# Parallaxa Camera Rig tests.
#
#   camera_model_test  pure motion model + stage_math, no libobs.
#   camera_obs_test    loads parallaxa.dll into a headless libobs with a
#                      D3D11 video context (installed OBS, nothing written to
#                      its config) and drives the rig through public OBS APIs.
#
# Standalone:
#   cmake -S src/camera/tests -B build-camera-tests -G Ninja
#         [-DSTAGE_PLUGIN_DLL=<parallaxa.dll> -DSTAGE_OBS_SRC=... -DSTAGE_IMPLIB_DIR=...]
#   cmake --build build-camera-tests && ctest --test-dir build-camera-tests
# From the plugin build: tests/camera adds this directory (the plugin target
# parallaxa is then used for camera_obs_test).
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(stage_camera_tests LANGUAGES C)
  set(CMAKE_C_STANDARD 17)
  set(CMAKE_C_STANDARD_REQUIRED ON)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  enable_testing()
endif()

set(_cam "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(_plugin "${_cam}/../..")

if(NOT TARGET stage_math)
  add_subdirectory("${_plugin}/src/math" "${CMAKE_CURRENT_BINARY_DIR}/stage_math")
endif()

add_executable(camera_model_test camera_model_test.c "${_cam}/stage-rig-model.c")
target_compile_definitions(camera_model_test PRIVATE STAGE_CORE_NO_OBS=1 _CRT_SECURE_NO_WARNINGS)
target_include_directories(camera_model_test PRIVATE "${_cam}" "${_plugin}/src/core" "${_plugin}/include")
target_link_libraries(camera_model_test PRIVATE stage_math)
set_target_properties(camera_model_test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(camera_model_test PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(camera_model_test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(camera_model_test PRIVATE m)
endif()
add_test(NAME camera_model_test COMMAND camera_model_test)

# Headless libobs test (Windows, installed OBS).
# Installed OBS Studio: -DSTAGE_OBS_DIR, else the OBS_DIR environment
# variable, else the default installer folder (tools/build.ps1 passes the
# folder release/obs-detect.ps1 finds).
if(DEFINED ENV{OBS_DIR})
  file(TO_CMAKE_PATH "$ENV{OBS_DIR}" _stage_obs_default)
else()
  set(_stage_obs_default "C:/Program Files/obs-studio")
endif()
set(STAGE_OBS_DIR "${_stage_obs_default}"
    CACHE PATH "Installed OBS Studio (camera_obs_test)")
set(STAGE_OBS_SRC "${_plugin}/third_party/obs-src" CACHE PATH "obs-studio source tree")
set(STAGE_IMPLIB_DIR "${_plugin}/third_party/implibs" CACHE PATH "obs.lib folder")
set(STAGE_PLUGIN_DLL "" CACHE FILEPATH "parallaxa.dll to test (standalone builds)")
if(TARGET parallaxa)
  set(_dll "$<TARGET_FILE:parallaxa>")
else()
  set(_dll "${STAGE_PLUGIN_DLL}")
endif()
if(WIN32 AND _dll AND EXISTS "${STAGE_OBS_DIR}/bin/64bit/obs.dll"
   AND EXISTS "${STAGE_IMPLIB_DIR}/obs.lib" AND EXISTS "${STAGE_OBS_SRC}/libobs/obs.h")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
  if(NOT EXISTS "${STAGE_OBS_SRC}/libobs/obsconfig.h")
    configure_file("${_plugin}/cmake/obsconfig.h.in" "${_gen}/obsconfig.h" COPYONLY)
  endif()
  add_executable(camera_obs_test camera_obs_test.c)
  target_compile_definitions(camera_obs_test PRIVATE HAVE_STRUCT_TIMESPEC _CRT_SECURE_NO_WARNINGS)
  target_include_directories(camera_obs_test PRIVATE "${_plugin}/include")
  target_include_directories(camera_obs_test SYSTEM PRIVATE "${_gen}" "${STAGE_OBS_SRC}/libobs"
    "${STAGE_OBS_SRC}/deps/w32-pthreads")
  target_link_libraries(camera_obs_test PRIVATE "${STAGE_IMPLIB_DIR}/obs.lib")
  set_target_properties(camera_obs_test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(camera_obs_test PRIVATE /W4 /WX /utf-8)
  endif()
  if(TARGET parallaxa)
    add_dependencies(camera_obs_test parallaxa)
  endif()
  add_test(NAME camera_obs_test COMMAND camera_obs_test "${_dll}" "${_plugin}/data" "${STAGE_OBS_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/work")
  set_tests_properties(camera_obs_test PROPERTIES
    TIMEOUT 180
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${STAGE_OBS_DIR}/bin/64bit")
endif()
