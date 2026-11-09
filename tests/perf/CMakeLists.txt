# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Performance harness (chapter 14 §14.9, WI-0.560). Google Benchmark binaries live in tests/perf/<area>/ and are
# built into <build>/tests/perf/<area>/, where tools/perf_run.py finds them (bench_*). The benchmark names and
# budgets are those of §14.9.3 (tests/perf/budgets.json, checked against the spec by tools/check_budgets.py).
#
#   ovt_add_benchmark(<area> <name> [LIBRARIES <target>...])
#       Builds tests/perf/<area>/<name>.cpp into the binary <name> (it must start with `bench_`), linked with
#       Google Benchmark, ovt::flags and LIBRARIES (module objects such as ovt_core_objects, as the unit tests
#       link them: ovt_core exports only the C API, #62). Adds the binary to the target `ovt_perf`.
#
# Run them on a reference machine with `cmake --build <dir> --target ovt_perf` and `tools/perf_run.py`
# (docs/dev/perf-runners.md). The CTest labels are `perf` only, so no PR lane runs them (the `perf` test preset
# does).

ovt_find_package(benchmark SPDX "Apache-2.0" TARGETS benchmark::benchmark OPTIONAL)
if(NOT benchmark_TARGET)
  message(STATUS "Google Benchmark not found: tests/perf not built (vcpkg feature 'benchmarks')")
  return()
endif()

set(_ovt_perf_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_ovt_perf_bin "${CMAKE_CURRENT_BINARY_DIR}")
add_custom_target(ovt_perf COMMENT "All Google Benchmark binaries (chapter 14 §14.9)")

function(ovt_add_benchmark area name)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "LIBRARIES")
  if(NOT name MATCHES "^bench_")
    message(
      FATAL_ERROR
        "ovt_add_benchmark: '${name}' must start with 'bench_' (tools/perf_run.py finds binaries by that prefix).")
  endif()
  add_executable(${name} "${_ovt_perf_src}/${area}/${name}.cpp")
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_ovt_perf_bin}/${area}")
  target_include_directories(${name} PRIVATE "${_ovt_perf_src}")
  target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} ovt::flags ${benchmark_TARGET})
  ovt_set_license(${name} "LGPL-2.1-or-later")
  add_dependencies(ovt_perf ${name})
endfunction()

add_subdirectory(core)

# The harness itself, end to end: run the smoke benchmark through tools/perf_run.py with short timings and read the
# result back with tools/perf_compare.py (hardware id `harness-test`: no budget column, no baseline). Needs
# Python only, so it also runs where no reference hardware exists.
if(Python3_Interpreter_FOUND)
  add_test(
    NAME perf_harness_smoke
    COMMAND
      "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/perf_run.py" --build-dir "${CMAKE_BINARY_DIR}" --filter
      "^bench_smoke$" --results-dir "${CMAKE_CURRENT_BINARY_DIR}/results" --hardware-id harness-test --min-time 0.02
      --repetitions 3 --compare)
  set_tests_properties(perf_harness_smoke PROPERTIES LABELS "perf" TIMEOUT 120)
endif()
