# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the metrics module (chapter 9 §9.2; WI-3.300). They link the module objects directly because
# ovt_core exports only the C API (#62 rule 2); the C API test links ovt::core instead, the way a consumer does.

add_subdirectory(example_plugin)

# ---- Module tests: ABI header (C11 + C++20), NormalizationConfig resolver, JobSpec parser, registry, job ----
add_executable(
  ovt_tests_metrics
  abi_header_c.c
  test_metric_abi.cpp
  test_metric_job.cpp
  test_metrics_jobspec.cpp
  test_metrics_plugin_registry.cpp
  test_metrics_psnr.cpp
  test_metrics_ssim.cpp
  test_metrics_vmaf.cpp
  test_normalization_config.cpp)
target_include_directories(ovt_tests_metrics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ovt_tests_metrics PRIVATE ovt_core_objects ovt_test_support ovt::flags ${flatbuffers_TARGET}
                                                ${nlohmann_json_TARGET} ${Catch2_TARGET})
target_compile_definitions(
  ovt_tests_metrics
  PRIVATE "OVT_EXAMPLE_METRIC_ROOT=\"${OVT_EXAMPLE_METRIC_ROOT}\""
          "OVT_METRICS_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\""
          "OVT_GOLDEN_METRICS_DIR=\"${PROJECT_SOURCE_DIR}/tests/golden/metrics\"")
set_source_files_properties(abi_header_c.c PROPERTIES LANGUAGE C)
set_target_properties(ovt_tests_metrics PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
get_property(_ovt_example_metrics GLOBAL PROPERTY OVT_EXAMPLE_METRIC_TARGETS)
add_dependencies(ovt_tests_metrics ${_ovt_example_metrics})
ovt_set_license(ovt_tests_metrics "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_metrics)

# ---- C API test: ovt/metrics.h through the shared library (acceptance test 3 via ovt_metrics_list_descriptors) --
add_executable(ovt_tests_metrics_c_api test_metrics_c_api.cpp)
target_link_libraries(ovt_tests_metrics_c_api PRIVATE ovt::core ovt::flags ovt_schemas ${flatbuffers_TARGET}
                                                      ${nlohmann_json_TARGET} ${Catch2_TARGET})
target_compile_definitions(ovt_tests_metrics_c_api PRIVATE "OVT_EXAMPLE_METRIC_ROOT=\"${OVT_EXAMPLE_METRIC_ROOT}\"")
add_dependencies(ovt_tests_metrics_c_api ${_ovt_example_metrics})
ovt_set_license(ovt_tests_metrics_c_api "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_metrics_c_api)
unset(_ovt_example_metrics)

# ---- The three type blocks ovt/abi/metric_v1.h repeats from the LGPL headers stay textually identical (#211) --
set(_ovt_inc "${PROJECT_SOURCE_DIR}/core/include/ovt")
set(_ovt_copies "${CMAKE_CURRENT_SOURCE_DIR}/check_header_copies.cmake")
add_test(NAME metrics_abi_status_copies_match
         COMMAND ${CMAKE_COMMAND} -Da=${_ovt_inc}/status.h -Db=${_ovt_inc}/abi/metric_v1.h
                 -Dguard=OVT_STATUS_TYPES_DEFINED -P "${_ovt_copies}")
add_test(NAME metrics_abi_frame_id_copies_match
         COMMAND ${CMAKE_COMMAND} -Da=${_ovt_inc}/decode.h -Db=${_ovt_inc}/abi/metric_v1.h -Dguard=OVT_FRAME_ID_DEFINED
                 -P "${_ovt_copies}")
add_test(NAME metrics_abi_color_copies_match
         COMMAND ${CMAKE_COMMAND} -Da=${_ovt_inc}/color.h -Db=${_ovt_inc}/abi/metric_v1.h
                 -Dguard=OVT_COLOR_DESCRIPTION_DEFINED -P "${_ovt_copies}")
set_tests_properties(metrics_abi_status_copies_match metrics_abi_frame_id_copies_match metrics_abi_color_copies_match
                     PROPERTIES LABELS "fast;unit;metrics")
unset(_ovt_inc)
unset(_ovt_copies)

# ---- core/src/metrics/vmaf/vmaf_models.inc is what tools/gen_vmaf_ids.py makes of the pinned libvmaf tree --------
# (WI-3.309 step 2). Needs the libvmaf sources, so only when third_party/libvmaf.cmake builds them (not with
# OVT_SYSTEM_LIBVMAF=ON).
if(TARGET tp_libvmaf)
  include(ExternalProject)
  externalproject_get_property(tp_libvmaf SOURCE_DIR)
  add_test(NAME metrics_vmaf_models_inc_current
           COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/gen_vmaf_ids.py" --libvmaf-src
                   "${SOURCE_DIR}" --check "${PROJECT_SOURCE_DIR}/core/src/metrics/vmaf/vmaf_models.inc")
  set_tests_properties(metrics_vmaf_models_inc_current PROPERTIES LABELS "fast;unit;metrics")
  unset(SOURCE_DIR)
  # Acceptance test 3 (WI-3.309): the Meson option set of chapter 9 §9.5.1 and, on arm64, vif_neon.c in the
  # introspection of the libvmaf build. The tp_libvmaf build runs the same check and fails without them.
  add_test(
    NAME metrics_vmaf_meson_introspection
    COMMAND
      ${CMAKE_COMMAND} "-DOVT_MESON=${OVT_MESON}" "-DBUILD_DIR=${OVT_LIBVMAF_BUILD_DIR}"
      "-DEXPECT=${OVT_LIBVMAF_EXPECT}" "-DREQUIRE_NEON=${OVT_LIBVMAF_REQUIRE_NEON}" -P
      "${OVT_LIBVMAF_INTROSPECT_SCRIPT}")
  set_tests_properties(metrics_vmaf_meson_introspection PROPERTIES LABELS "fast;unit;metrics")
endif()

# ---- The introspection check itself, on recorded `meson introspect` output (runs without a libvmaf build) ------
set(_ovt_vi "${CMAKE_CURRENT_SOURCE_DIR}/vmaf_introspect")
set(_ovt_vi_expect "buildtype=release|default_library=static|enable_float=true|enable_avx512=true|"
                   "built_in_models=true|enable_cuda=false|enable_nvtx=false|enable_docs=false")
string(JOIN "" _ovt_vi_expect ${_ovt_vi_expect})
foreach(
  _case IN
  ITEMS "arm64;targets_arm64;ON;${_ovt_vi_expect};0" "x86;targets_scalar;OFF;${_ovt_vi_expect};0"
        "no_neon;targets_scalar;ON;${_ovt_vi_expect};1" "option;targets_arm64;OFF;enable_float=false;1"
        "unknown;targets_arm64;OFF;enable_zimg=true;1")
  list(GET _case 0 _name)
  list(GET _case 1 _targets)
  list(GET _case 2 _neon)
  list(GET _case 3 _expect)
  list(GET _case 4 _fails)
  add_test(
    NAME metrics_vmaf_introspect_check_${_name}
    COMMAND ${CMAKE_COMMAND} "-DOPTIONS_JSON=${_ovt_vi}/buildoptions.json" "-DTARGETS_JSON=${_ovt_vi}/${_targets}.json"
            "-DEXPECT=${_expect}" "-DREQUIRE_NEON=${_neon}" -P "${PROJECT_SOURCE_DIR}/cmake/OvtLibvmafIntrospect.cmake")
  set_tests_properties(metrics_vmaf_introspect_check_${_name} PROPERTIES LABELS "fast;unit;metrics" WILL_FAIL ${_fails})
endforeach()
unset(_case)
unset(_name)
unset(_targets)
unset(_neon)
unset(_expect)
unset(_fails)
unset(_ovt_vi_expect)
unset(_ovt_vi)
