# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of libovt-core (chapter 14 §14.2, WI-0.550).
#
# Catch2 linkage rule (D24, chapter 14 §14.2.1): every test executable links Catch2::Catch2WithMain and
# has no main of its own, except the golden runner ovt_core_tests, which links Catch2::Catch2 and brings
# main.cpp (logger hook, OVT_UPDATE_GOLDEN / OVT_REQUIRE_CORPORA / OVT_CORPORA_DIR).
#
# Tests are registered with ovt_catch_discover_tests() (cmake/OvtTesting.cmake): Catch2 tags become CTest
# labels, so the lane presets of CMakePresets.json (pr = label fast, …) select them. Every test case
# carries exactly one speed tag, [fast] or [slow] (chapter 14 §14.2.5).
#
# A module's tests go in core/tests/<module>/ with their own executable ovt_tests_<module> (linking
# ovt_test_support and Catch2WithMain), or into ovt_core_tests when they compare golden files.

ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
if(NOT TARGET Catch2::Catch2)
  message(FATAL_ERROR "Catch2::Catch2 (without main) is required for the golden runner ovt_core_tests.")
endif()

# ---- Test support library: corpus lookup, golden comparison, normalisation -----------------------
add_library(ovt_test_support STATIC test_support/corpus.cpp test_support/env.cpp test_support/fb_normalize.cpp
                                    test_support/golden.cpp test_support/golden_compare.cpp)
target_include_directories(ovt_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ovt_test_support PRIVATE "OVT_TEST_DEFAULT_CORPORA_DIR=\"${OVT_CORPORA_DIR}\"")
# Catch2::Catch2 (not WithMain): the support code uses SKIP/FAIL/WARN; the executable decides about main.
# nlohmann_json is PUBLIC: golden_compare.h and fb_normalize.h expose nlohmann::json (WI-0.556). ovt_schemas (the
# generated FlatBuffers headers) is private: the family writers read the tables, callers see forward declarations.
target_link_libraries(ovt_test_support PUBLIC Catch2::Catch2 ${nlohmann_json_TARGET}
                      PRIVATE ovt::flags ${fmt_TARGET} ${flatbuffers_TARGET} ovt_schemas)
ovt_set_license(ovt_test_support "LGPL-2.1-or-later")

# ---- Golden runner: the one test executable with its own main -------------------------------------
add_executable(ovt_core_tests main.cpp test_support_corpus.cpp test_support_flatbuffer.cpp test_support_golden.cpp
                              test_golden_compare.cpp test_fb_normalize_families.cpp)
target_link_libraries(ovt_core_tests PRIVATE ovt::core ovt::flags ovt_test_support ovt_schemas Catch2::Catch2
                                             ${flatbuffers_TARGET} ${nlohmann_json_TARGET})
target_compile_definitions(ovt_core_tests PRIVATE "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
                                                  "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_core_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_core_tests)

# ---- Ordinary test executables (Catch2WithMain) ---------------------------------------------------
add_executable(ovt_tests_version test_version.cpp)
target_link_libraries(ovt_tests_version PRIVATE ovt::core ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_version "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_version)

# ovt::Result<T> (WI-1.512): header only; linked against the module objects for their include paths.
add_executable(ovt_tests_result test_result.cpp)
target_link_libraries(ovt_tests_result PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_result "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_result)

# ---- Module tests ---------------------------------------------------------------------------------
add_subdirectory(io)
add_subdirectory(analysis)
add_subdirectory(bitreader)
add_subdirectory(fuzz) # WI-0.557: libFuzzer targets and their replay tests
add_subdirectory(golden) # WI-0.093: hand-built FrameAnalysis fixtures and their generator
add_subdirectory(jobs)
add_subdirectory(log)
add_subdirectory(model)
add_subdirectory(syntax)

# ---- Schema code generation (WI-0.003) ---------------------------------------------------------------
add_subdirectory(schema)

# ---- Third-party probes (build scripts of chapter 13 §13.3) ---------------------------------------
# WI-0.011: tp::dav1d (Meson wrapper) links into a test that calls dav1d_version().
if(TARGET tp::dav1d)
  add_executable(ovt_tests_dav1d_probe test_dav1d_probe.cpp)
  target_link_libraries(ovt_tests_dav1d_probe PRIVATE tp::dav1d ovt::flags ${Catch2_TARGET})
  ovt_set_license(ovt_tests_dav1d_probe "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_tests_dav1d_probe)
endif()

# WI-0.013: tp::aom (bundled libaom with CONFIG_INSPECTION) is the pinned version and accepts the inspection
# callback. aom_inspection_check is acceptance test 2: the OVT_SYSTEM_LIBAOM configure check rejects a libaom
# without inspection (fake_libaom) and accepts the bundled one. Both need the bundled build, so they are not
# registered with OVT_SYSTEM_LIBAOM=ON; the configure check cannot run a probe in a cross build.
if(TARGET tp::aom AND TARGET tp_libaom)
  add_executable(ovt_tests_aom_probe test_aom_probe.cpp)
  target_link_libraries(ovt_tests_aom_probe PRIVATE tp::aom ovt::flags ${Catch2_TARGET})
  target_compile_definitions(ovt_tests_aom_probe PRIVATE "OVT_AOM_PINNED_VERSION=\"${OVT_TP_libaom_VERSION}\"")
  ovt_set_license(ovt_tests_aom_probe "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_tests_aom_probe)

  if(NOT CMAKE_CROSSCOMPILING)
    add_library(ovt_test_fake_aom STATIC aom_inspection_check/fake_libaom.c)
    target_link_libraries(ovt_test_fake_aom PRIVATE tp::aom ovt::flags)
    ovt_set_license(ovt_test_fake_aom "LGPL-2.1-or-later")
    get_target_property(_ovt_aom_include tp::aom INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_ovt_aom_library tp::aom IMPORTED_LOCATION)
    add_test(
      NAME aom_inspection_check
      COMMAND
        "${CMAKE_COMMAND}" -D "OVT_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake" -D
        "CHECK_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/aom_inspection_check" -D
        "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/aom_inspection_check" -D "GENERATOR=${CMAKE_GENERATOR}" -D
        "C_COMPILER=${CMAKE_C_COMPILER}" -D "C_FLAGS=${OVT_TP_SANITIZER_C_FLAGS}" -D
        "LINK_FLAGS=${OVT_TP_SANITIZER_LINK_FLAGS}" -D "AOM_INCLUDE_DIR=${_ovt_aom_include}" -D
        "FAKE_LIBRARY=$<TARGET_FILE:ovt_test_fake_aom>" -D "REAL_LIBRARY=${_ovt_aom_library}" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/aom_inspection_check/run_check.cmake")
    # Two configures of a one-file C project: a few seconds, so it runs in the fast lane.
    set_tests_properties(aom_inspection_check PROPERTIES LABELS "fast;third_party" TIMEOUT 300)
    unset(_ovt_aom_include)
    unset(_ovt_aom_library)
  endif()
endif()

# WI-0.012: the two FFmpeg flavours (chapter 13 §13.3.3). One source, one executable per flavour: both define the
# av* symbols, so they cannot be linked into one process outside the plugin.
foreach(_ovt_ff_flavour core plugin)
  if(NOT TARGET tp::ffmpeg_${_ovt_ff_flavour})
    continue()
  endif()
  if(_ovt_ff_flavour STREQUAL "core")
    set(_ovt_ff_test ovt_tests_ffmpeg_probe)
    set(_ovt_ff_is_plugin 0)
  else()
    set(_ovt_ff_test ovt_tests_ffmpeg_plugin_probe)
    set(_ovt_ff_is_plugin 1)
  endif()
  add_executable(${_ovt_ff_test} test_ffmpeg_probe.cpp)
  target_link_libraries(${_ovt_ff_test} PRIVATE tp::ffmpeg_${_ovt_ff_flavour} ovt_test_support ovt::flags
                                                ${Catch2_TARGET})
  set(_ovt_ff_defs "OVT_FFMPEG_PLUGIN_FLAVOUR=${_ovt_ff_is_plugin}")
  foreach(_ovt_ff_opt OVT_SYSTEM_FFMPEG OVT_PATENT_CODECS_IN_CORE OVT_ENABLE_HW_DECODE)
    if(${_ovt_ff_opt})
      set(_ovt_ff_on 1)
    else()
      set(_ovt_ff_on 0)
    endif()
    string(REPLACE "OVT_SYSTEM_FFMPEG" "OVT_FFMPEG_SYSTEM" _ovt_ff_name "${_ovt_ff_opt}")
    list(APPEND _ovt_ff_defs "${_ovt_ff_name}=${_ovt_ff_on}")
  endforeach()
  if(TARGET tp::dav1d)
    list(APPEND _ovt_ff_defs OVT_FFMPEG_DAV1D=1)
  else()
    list(APPEND _ovt_ff_defs OVT_FFMPEG_DAV1D=0)
  endif()
  target_compile_definitions(${_ovt_ff_test} PRIVATE ${_ovt_ff_defs})
  ovt_set_license(${_ovt_ff_test} "LGPL-2.1-or-later")
  ovt_catch_discover_tests(${_ovt_ff_test})
endforeach()
unset(_ovt_ff_flavour)
unset(_ovt_ff_test)
unset(_ovt_ff_is_plugin)
unset(_ovt_ff_defs)
unset(_ovt_ff_opt)
unset(_ovt_ff_on)
unset(_ovt_ff_name)

# WI-0.015: tp::vmaf (Meson wrapper) links into a test that loads the built-in vmaf_v0.6.1 model.
if(TARGET tp::vmaf)
  add_executable(ovt_tests_vmaf_probe test_vmaf_probe.cpp)
  target_link_libraries(ovt_tests_vmaf_probe PRIVATE tp::vmaf ovt::flags ${Catch2_TARGET})
  ovt_set_license(ovt_tests_vmaf_probe "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_tests_vmaf_probe)
endif()

# WI-0.017: tp::onnxruntime (prebuilt fallback, OVT_ONNXRUNTIME_PREBUILT=ON) links into a plain C program that
# calls OrtGetApiBase(); it exits 0 when the header is found and the library loads.
if(TARGET tp::onnxruntime)
  add_executable(ovt_tests_ort_probe test_ort_probe.c)
  target_link_libraries(ovt_tests_ort_probe PRIVATE tp::onnxruntime ovt::flags)
  ovt_set_license(ovt_tests_ort_probe "LGPL-2.1-or-later")
  add_test(NAME ort_probe COMMAND ovt_tests_ort_probe)
  set_tests_properties(ort_probe PROPERTIES LABELS "unit;fast;third_party")
endif()
