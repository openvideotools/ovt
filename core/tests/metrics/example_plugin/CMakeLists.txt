# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The dummy metric plugin of WI-3.300 (libovt_metric_example, acceptance test 3) and the broken variants the
# registry tests load: the same source with other knobs (example_plugin.cpp), each in its own directory.
# ovt_add_example_metric(<target> DIR <subdir> ID <file id> [DEFINES <defs>...]) builds
# ${OVT_EXAMPLE_METRIC_ROOT}/<subdir>/libovt_metric_<file id>. WI-3.318 adds the real example plugin under
# plugins/ with its developer guide; this one stays a test fixture.

set(OVT_EXAMPLE_METRIC_ROOT "${CMAKE_CURRENT_BINARY_DIR}" CACHE INTERNAL
                                                                "Root of the example metric plugin directories")

function(ovt_add_example_metric target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "DIR;ID" "DEFINES")
  add_library(${target} MODULE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/example_plugin.cpp")
  # A plugin links no part of libovt-core (chapter 0 §0.6.3): ABI header and generated FlatBuffers headers only.
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
  target_link_libraries(${target} PRIVATE ovt_schemas ovt::flags)
  target_compile_definitions(${target} PRIVATE ${arg_DEFINES})
  set_target_properties(
    ${target}
    PROPERTIES PREFIX "lib"
               OUTPUT_NAME "ovt_metric_${arg_ID}"
               SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
               LIBRARY_OUTPUT_DIRECTORY "${OVT_EXAMPLE_METRIC_ROOT}/${arg_DIR}"
               C_VISIBILITY_PRESET hidden
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON)
  ovt_set_license(${target} "LGPL-2.1-or-later")
  set_property(GLOBAL APPEND PROPERTY OVT_EXAMPLE_METRIC_TARGETS ${target})
endfunction()

# Acceptance test 3: the working plugin. The rest are the rejection cases of the registry tests.
ovt_add_example_metric(ovt_metric_example DIR example ID example)
ovt_add_example_metric(ovt_metric_example_dup DIR example_dup ID example)
ovt_add_example_metric(ovt_metric_example_abi2 DIR bad_abi ID abi2 DEFINES "OVT_EXAMPLE_ID=\"abi2\""
                       OVT_EXAMPLE_ABI_VERSION=2)
ovt_add_example_metric(ovt_metric_example_wrong_id DIR bad_id ID wrongid DEFINES "OVT_EXAMPLE_ID=\"other\"")
ovt_add_example_metric(ovt_metric_example_small DIR bad_size ID small DEFINES "OVT_EXAMPLE_ID=\"small\""
                       OVT_EXAMPLE_API_SIZE=16)
ovt_add_example_metric(ovt_metric_example_null DIR bad_null ID nullscore DEFINES "OVT_EXAMPLE_ID=\"nullscore\""
                       OVT_EXAMPLE_NULL_SCORE)
ovt_add_example_metric(ovt_metric_example_bad_desc DIR bad_desc ID baddesc DEFINES "OVT_EXAMPLE_ID=\"baddesc\""
                       OVT_EXAMPLE_BAD_DESCRIPTOR)
ovt_add_example_metric(ovt_metric_example_both DIR bad_both ID both DEFINES "OVT_EXAMPLE_ID=\"both\""
                       OVT_EXAMPLE_BOTH_ENTRY_POINTS)
