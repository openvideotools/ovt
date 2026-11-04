# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# cmake -D OVT_GENERATED_DIR=<build>/generated -P check_generated_outputs.cmake
# Fails unless the ovt_schemas outputs for ovt_model.fbs exist at the chapter 13 §13.2.3 paths
# (WI-0.080 acceptance test 1).

if(NOT DEFINED OVT_GENERATED_DIR)
  message(FATAL_ERROR "OVT_GENERATED_DIR is not set")
endif()

set(_missing "")
foreach(_path "${OVT_GENERATED_DIR}/ovt_model_generated.h" "${OVT_GENERATED_DIR}/python/ovt/fb/FrameAnalysis.py"
              "${OVT_GENERATED_DIR}/python/ovt/fb/__init__.py")
  if(NOT EXISTS "${_path}")
    list(APPEND _missing "${_path}")
  endif()
endforeach()

file(GLOB_RECURSE _kotlin "${OVT_GENERATED_DIR}/kotlin/*.kt")
if(NOT _kotlin)
  list(APPEND _missing "${OVT_GENERATED_DIR}/kotlin/**/*.kt")
endif()

if(_missing)
  string(REPLACE ";" "\n  " _missing "${_missing}")
  message(FATAL_ERROR "ovt_schemas outputs missing:\n  ${_missing}")
endif()
message(STATUS "ovt_model.fbs outputs present under ${OVT_GENERATED_DIR}")
