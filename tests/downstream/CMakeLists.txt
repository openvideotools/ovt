# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Two roles (WI-0.008, chapter 13 §13.2.6):
#
# 1. Inside the OVT build (added by tests/CMakeLists.txt): registers the CTest test downstream_consumer,
#    which installs the build into a temporary prefix and builds this directory against it as a separate
#    project (run_downstream.cmake).
# 2. As a standalone project (what run_downstream.cmake configures): a C11 consumer that uses only
#    find_package(ovt 1 CONFIG REQUIRED) and ovt::core, with no vcpkg toolchain.

if(NOT PROJECT_NAME STREQUAL "OpenVideoTools")
  cmake_minimum_required(VERSION 3.25)
  project(ovt_downstream_consumer LANGUAGES C)

  find_package(ovt 1 CONFIG REQUIRED)
  message(STATUS "ovt package ${ovt_VERSION} from ${ovt_DIR}; OVT_PLUGIN_DIR=${OVT_PLUGIN_DIR}")

  add_executable(ovt_downstream_consumer main.c)
  set_target_properties(ovt_downstream_consumer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(ovt_downstream_consumer PRIVATE ovt::core)
  return()
endif()

add_test(NAME downstream_consumer
         COMMAND "${CMAKE_COMMAND}"
           -D "OVT_BUILD_DIR=${CMAKE_BINARY_DIR}"
           -D "OVT_CONFIG=$<CONFIG>"
           -D "CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
           -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/work"
           -D "GENERATOR=${CMAKE_GENERATOR}"
           -D "C_COMPILER=${CMAKE_C_COMPILER}"
           -D "EXPECTED_VERSION=${PROJECT_VERSION}${OVT_VERSION_SUFFIX}"
           -P "${CMAKE_CURRENT_SOURCE_DIR}/run_downstream.cmake")
# Under a second (install + configure + build of one C file), so it runs in the fast lane on every
# CI platform (WI-0.008 acceptance test: macOS and Linux).
set_tests_properties(downstream_consumer PROPERTIES LABELS "fast;install" TIMEOUT 300)
