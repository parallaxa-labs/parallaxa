# Fails when any file in MATH_DIR includes an OBS header (obs*.h, libobs/,
# graphics/, util/, callback/, media-io/) — stage_math must stay libobs-free.
#
# Copyright (C) 2026 Parallaxa Labs
# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT MATH_DIR)
  message(FATAL_ERROR "MATH_DIR not set")
endif()
file(GLOB _files "${MATH_DIR}/*.c" "${MATH_DIR}/*.h")
if(NOT _files)
  message(FATAL_ERROR "no sources found in ${MATH_DIR}")
endif()
set(_bad "")
foreach(_f IN LISTS _files)
  file(STRINGS "${_f}" _hits
    REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](obs|libobs/|graphics/|util/|callback/|media-io/)")
  if(_hits)
    list(APPEND _bad "${_f}: ${_hits}")
  endif()
endforeach()
if(_bad)
  message(FATAL_ERROR "OBS includes found in stage_math:\n${_bad}")
endif()
message(STATUS "stage_math: no OBS includes in ${MATH_DIR}")
