# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# cmake -D OVT_GENERATED_DIR=<build>/generated -P check_generated_outputs.cmake
# Fails unless the ovt_schemas outputs for ovt_model.fbs, ovt_container.fbs and ovt_metrics.fbs exist at the
# chapter 13 §13.2.3 paths (acceptance test 1 of WI-0.080, WI-0.081 and WI-0.082).

if(NOT DEFINED OVT_GENERATED_DIR)
  message(FATAL_ERROR "OVT_GENERATED_DIR is not set")
endif()

set(_missing "")
foreach(
  _path
  "${OVT_GENERATED_DIR}/ovt_model_generated.h"
  "${OVT_GENERATED_DIR}/python/ovt/fb/FrameAnalysis.py"
  "${OVT_GENERATED_DIR}/python/ovt/fb/__init__.py"
  "${OVT_GENERATED_DIR}/ovt_container_generated.h"
  "${OVT_GENERATED_DIR}/python/ovt/fb/ContainerRoot.py"
  "${OVT_GENERATED_DIR}/ovt_metrics_generated.h"
  "${OVT_GENERATED_DIR}/python/ovt/fb/MetricsReport.py")
  if(NOT EXISTS "${_path}")
    list(APPEND _missing "${_path}")
  endif()
endforeach()

file(GLOB_RECURSE _kotlin "${OVT_GENERATED_DIR}/kotlin/*.kt")
if(NOT _kotlin)
  list(APPEND _missing "${OVT_GENERATED_DIR}/kotlin/**/*.kt")
endif()
file(GLOB_RECURSE _kotlin_root "${OVT_GENERATED_DIR}/kotlin/ContainerRoot.kt")
if(NOT _kotlin_root)
  list(APPEND _missing "${OVT_GENERATED_DIR}/kotlin/**/ContainerRoot.kt")
endif()

file(GLOB_RECURSE _kotlin_report "${OVT_GENERATED_DIR}/kotlin/MetricsReport.kt")
if(NOT _kotlin_report)
  list(APPEND _missing "${OVT_GENERATED_DIR}/kotlin/**/MetricsReport.kt")
endif()

if(_missing)
  string(REPLACE ";" "\n  " _missing "${_missing}")
  message(FATAL_ERROR "ovt_schemas outputs missing:\n  ${_missing}")
endif()
message(STATUS "ovt_model.fbs, ovt_container.fbs and ovt_metrics.fbs outputs present under ${OVT_GENERATED_DIR}")
