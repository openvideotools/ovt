# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The mock analysis plugin of WI-2.150 (libovt_plugin_mock, also for the GUI tests) and the broken variants the
# registry tests load: the same source with other caps/ABI knobs (mock_plugin.cpp), each in its own directory.
# ovt_add_mock_plugin(<target> DIR <subdir> ID <file id> [DEFINES <defs>...]) builds
# ${OVT_MOCK_PLUGIN_ROOT}/<subdir>/libovt_plugin_<file id>.

set(OVT_MOCK_PLUGIN_ROOT "${CMAKE_CURRENT_BINARY_DIR}" CACHE INTERNAL "Root of the mock plugin directories")

function(ovt_add_mock_plugin target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "DIR;ID" "DEFINES")
  add_library(${target} MODULE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mock_plugin.cpp")
  # A plugin links no part of libovt-core (chapter 0 §0.6.2): ABI header and generated FlatBuffers headers only.
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
  target_link_libraries(${target} PRIVATE ovt_schemas ovt::flags)
  target_compile_definitions(${target} PRIVATE ${arg_DEFINES})
  set_target_properties(
    ${target}
    PROPERTIES PREFIX "lib"
               OUTPUT_NAME "ovt_plugin_${arg_ID}"
               SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
               LIBRARY_OUTPUT_DIRECTORY "${OVT_MOCK_PLUGIN_ROOT}/${arg_DIR}"
               C_VISIBILITY_PRESET hidden
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON)
  ovt_set_license(${target} "LGPL-2.1-or-later")
  set_property(GLOBAL APPEND PROPERTY OVT_MOCK_PLUGIN_TARGETS ${target})
endfunction()

# Acceptance tests 1, 3 (same id, another version) and 2 (abi_version 2); also the GUI tests.
ovt_add_mock_plugin(ovt_plugin_mock DIR mock ID mock)
ovt_add_mock_plugin(ovt_plugin_mock_dup DIR mock_dup ID mock DEFINES "OVT_MOCK_VERSION=\"2.0.0-duplicate\"")
ovt_add_mock_plugin(ovt_plugin_mock_abi2 DIR bad_abi ID abi2 DEFINES "OVT_MOCK_ID=\"abi2\"" OVT_MOCK_ABI_VERSION=2)
# Rejected: wrong caps->id, bad codec, struct_size < v1, NULL entry. mock_hevc: an H265 id for default_for().
ovt_add_mock_plugin(ovt_plugin_mock_wrong_id DIR bad_id ID wrongid DEFINES "OVT_MOCK_ID=\"other\"")
ovt_add_mock_plugin(ovt_plugin_mock_bad_codec DIR bad_codec ID badcodec DEFINES "OVT_MOCK_ID=\"badcodec\""
                    OVT_MOCK_CODEC=200)
ovt_add_mock_plugin(ovt_plugin_mock_hevc DIR hevc ID mockhevc DEFINES "OVT_MOCK_ID=\"mockhevc\"" OVT_MOCK_CODEC=2)
ovt_add_mock_plugin(ovt_plugin_mock_av1 DIR av1 ID mockav1 DEFINES "OVT_MOCK_ID=\"mockav1\"" OVT_MOCK_CODEC=3)
ovt_add_mock_plugin(ovt_plugin_mock_small DIR bad_size ID small DEFINES "OVT_MOCK_ID=\"small\"" OVT_MOCK_API_SIZE=16)
ovt_add_mock_plugin(ovt_plugin_mock_null DIR bad_null ID nullentry DEFINES "OVT_MOCK_ID=\"nullentry\""
                    OVT_MOCK_NULL_FLUSH)
