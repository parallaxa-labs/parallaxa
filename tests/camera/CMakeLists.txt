# Camera rig tests. They live next to the module in
# src/camera/tests; this directory only puts them in the tests/ glob so the
# plugin build's ctest (tools/build.ps1) runs them. Standalone builds get
# camera_model_test only (camera_obs_test needs the plugin target or
# -DSTAGE_PLUGIN_DLL).
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/camera/tests" "${CMAKE_CURRENT_BINARY_DIR}/src")

# Guard: every src/<mod>/tests folder must be added from a tests/* directory.
add_test(NAME module_tests_wired
  COMMAND "${CMAKE_COMMAND}" "-DPLUGIN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../.."
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check-module-tests.cmake")
