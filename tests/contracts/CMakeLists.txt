# Contract checks that need only Node.js 18+ (no OBS, no compiler):
#   protocol_parity   include/stage-protocol.h against docs/protocol
#                     (names.vendor.json, vendor-tables.json), plus its self-test
#   presets_check     data/presets against the JSON Schemas, cross references,
#                     LUT images, the sample scene collection and
#                     docs/protocol/builtin-presets.json
# Skipped (with a message) when node is not on PATH.
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
find_program(STAGE_NODE node)
if(NOT STAGE_NODE)
  message(STATUS "node not found: protocol_parity and presets_check are skipped")
  return()
endif()

add_test(NAME protocol_parity
  COMMAND "${STAGE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/protocol_check.mjs")
add_test(NAME protocol_parity_selftest
  COMMAND "${STAGE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/protocol_check.mjs" --selftest)
add_test(NAME presets_check
  COMMAND "${STAGE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/presets_check.mjs")
set_tests_properties(protocol_parity protocol_parity_selftest presets_check PROPERTIES TIMEOUT 60)
