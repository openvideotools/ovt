# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Stand-alone project configured by run_check.cmake (WI-0.013 acceptance test 2): runs the OVT_SYSTEM_LIBAOM
# inspection check of cmake/OvtAomInspection.cmake against the libaom given as AOM_LIBRARY/AOM_INCLUDE_DIR.

cmake_minimum_required(VERSION 3.25)
project(ovt_aom_inspection_check LANGUAGES C)

foreach(var OVT_CMAKE_DIR AOM_LIBRARY AOM_INCLUDE_DIR)
  if(NOT ${var})
    message(FATAL_ERROR "${var} is required.")
  endif()
endforeach()
list(APPEND CMAKE_MODULE_PATH "${OVT_CMAKE_DIR}")

find_package(Threads REQUIRED)
add_library(aom_under_test STATIC IMPORTED)
set(libs Threads::Threads)
if(NOT APPLE)
  list(APPEND libs m)
endif()
set_target_properties(
  aom_under_test PROPERTIES IMPORTED_LOCATION "${AOM_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${AOM_INCLUDE_DIR}"
                            INTERFACE_LINK_LIBRARIES "${libs}")

include(OvtAomInspection)
ovt_aom_require_inspection(aom_under_test)
