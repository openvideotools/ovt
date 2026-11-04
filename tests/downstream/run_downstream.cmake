# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Script mode (cmake -P), run by the CTest test downstream_consumer (WI-0.008):
#   1. cmake --install the OVT build tree into WORK_DIR/prefix;
#   2. check that the exported targets leak no build-time dependency (static absorption, §13.2.6);
#   3. configure CONSUMER_SOURCE_DIR as its own project with CMAKE_PREFIX_PATH = that prefix only, no
#      toolchain file and no vcpkg variables, then build and run it;
#   4. require its output to contain the expected version.
#
# Inputs (-D): OVT_BUILD_DIR, OVT_CONFIG, CONSUMER_SOURCE_DIR, WORK_DIR, GENERATOR, C_COMPILER,
# EXPECTED_VERSION.

foreach(var OVT_BUILD_DIR CONSUMER_SOURCE_DIR WORK_DIR GENERATOR EXPECTED_VERSION)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "run_downstream.cmake: ${var} is not set")
  endif()
endforeach()

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    string(JOIN " " cmd ${ARGN})
    message(FATAL_ERROR "downstream_consumer: command failed (${rc}): ${cmd}\n${out}\n${err}")
  endif()
  set(run_output "${out}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
set(prefix "${WORK_DIR}/prefix")
set(build "${WORK_DIR}/build")

# 1. Install.
set(config_args "")
if(OVT_CONFIG)
  set(config_args --config "${OVT_CONFIG}")
endif()
run("${CMAKE_COMMAND}" --install "${OVT_BUILD_DIR}" --prefix "${prefix}" ${config_args})

# 2. No vcpkg or other build-time target may appear in the exported link interface. Only
#    INTERFACE_LINK_LIBRARIES matters to consumers; IMPORTED_LINK_DEPENDENT_LIBRARIES (written when a system
#    build links shared fmt/spdlog) only feeds the linker's runtime search path.
file(GLOB_RECURSE targets_files "${prefix}/*/ovt-targets*.cmake")
if(NOT targets_files)
  message(FATAL_ERROR "downstream_consumer: no ovt-targets.cmake under ${prefix}")
endif()
foreach(f IN LISTS targets_files)
  file(STRINGS "${f}" lines REGEX "INTERFACE_LINK_LIBRARIES")
  foreach(line IN LISTS lines)
    foreach(leak "fmt::" "spdlog::" "flatbuffers::" "zstd::" "nlohmann_json::" "hwy::" "xxHash::" "CLI11::"
                 "Catch2::" "vcpkg_installed" "ovt::flags" "ovt_flags")
      string(FIND "${line}" "${leak}" pos)
      if(NOT pos EQUAL -1)
        message(FATAL_ERROR "downstream_consumer: ${f} exports '${leak}' in INTERFACE_LINK_LIBRARIES; private "
                            "dependencies must stay PRIVATE or $<BUILD_INTERFACE:...> (chapter 13 §13.2.6)")
      endif()
    endforeach()
  endforeach()
endforeach()

# 3. Configure, build and run the consumer as an unrelated project. The environment of this script
#    may carry vcpkg variables from the outer build; clear them for the child.
foreach(env VCPKG_ROOT VCPKG_FEATURE_FLAGS VCPKG_OVERLAY_PORTS VCPKG_OVERLAY_TRIPLETS CMAKE_TOOLCHAIN_FILE
            CMAKE_PREFIX_PATH)
  unset(ENV{${env}})
endforeach()
set(compiler_args "")
if(C_COMPILER)
  set(compiler_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
run("${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE_DIR}" -B "${build}" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${prefix}" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF -DCMAKE_BUILD_TYPE=Release ${compiler_args})
message(STATUS "${run_output}")
run("${CMAKE_COMMAND}" --build "${build}" --config Release)

find_program(consumer ovt_downstream_consumer PATHS "${build}" "${build}/Release" NO_DEFAULT_PATH)
if(NOT consumer)
  message(FATAL_ERROR "downstream_consumer: the consumer executable was not built in ${build}")
endif()
run("${consumer}")
message(STATUS "${run_output}")

# 4. The consumer must report the version of the build it was installed from.
string(FIND "${run_output}" "ovt_version_string: ${EXPECTED_VERSION}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "downstream_consumer: expected 'ovt_version_string: ${EXPECTED_VERSION}', got: ${run_output}")
endif()
message(STATUS "downstream_consumer: OK")
