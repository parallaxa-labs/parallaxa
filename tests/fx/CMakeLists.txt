# Parallaxa FX tests.
#
#   stage_fx_params   pure C (no libobs): src/fx/stage-fx-params.c + stage_math
#   stage_fx_gpu_d3d11 / stage_fx_gpu_opengl
#                     headless libobs render tests of stage_fx and
#                     stage_depth_blur (plugin build only, needs an installed
#                     OBS; nothing is written outside the build folder)
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT TARGET stage_math)
  message(STATUS "stage_math not present: skipping fx tests")
  return()
endif()

set(_plugin "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(_fx "${_plugin}/src/fx")

add_executable(fx_params_test fx_params_test.c "${_fx}/stage-fx-params.c")
target_include_directories(fx_params_test PRIVATE "${_fx}" "${_plugin}/include")
target_link_libraries(fx_params_test PRIVATE stage_math)
target_compile_definitions(fx_params_test PRIVATE _CRT_SECURE_NO_WARNINGS) # standalone: the root sets it globally
set_target_properties(fx_params_test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(fx_params_test PRIVATE /W4 /WX /utf-8)
else()
  target_compile_options(fx_params_test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(fx_params_test PRIVATE m)
endif()
add_test(NAME stage_fx_params COMMAND fx_params_test)

# Headless GPU tests: only in the plugin build and with an OBS install.
if(NOT STAGE_OBS_DIR)
  # Installed OBS Studio: -DSTAGE_OBS_DIR, else the OBS_DIR environment
  # variable, else the default installer folder (tools/build.ps1 passes the
  # folder release/obs-detect.ps1 finds).
  if(DEFINED ENV{OBS_DIR})
    file(TO_CMAKE_PATH "$ENV{OBS_DIR}" _stage_obs_default)
  else()
    set(_stage_obs_default "C:/Program Files/obs-studio")
  endif()
  set(STAGE_OBS_DIR "${_stage_obs_default}")
endif()
if(TARGET parallaxa AND WIN32 AND EXISTS "${STAGE_OBS_DIR}/bin/64bit/obs.dll")
  add_executable(fx_gpu_test fx_gpu_test.c)
  target_include_directories(fx_gpu_test SYSTEM PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    "${STAGE_OBS_SRC}/libobs"
    "${STAGE_OBS_SRC}/deps/w32-pthreads")
  target_compile_definitions(fx_gpu_test PRIVATE HAVE_STRUCT_TIMESPEC _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(fx_gpu_test PRIVATE
    "${STAGE_IMPLIB_DIR}/obs.lib" "${STAGE_IMPLIB_DIR}/w32-pthreads.lib")
  set_target_properties(fx_gpu_test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(fx_gpu_test PRIVATE /W4 /WX /utf-8)
  endif()
  add_dependencies(fx_gpu_test parallaxa)

  foreach(_renderer IN ITEMS d3d11 opengl)
    set(_work "${CMAKE_CURRENT_BINARY_DIR}/gpu-${_renderer}")
    add_test(NAME stage_fx_gpu_${_renderer}
      COMMAND fx_gpu_test "$<TARGET_FILE:parallaxa>" "${_plugin}/data"
              "${STAGE_OBS_DIR}/bin/64bit" ${_renderer} "${_work}")
    set_tests_properties(stage_fx_gpu_${_renderer} PROPERTIES
      TIMEOUT 180
      RUN_SERIAL TRUE
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${STAGE_OBS_DIR}/bin/64bit")
  endforeach()
endif()
