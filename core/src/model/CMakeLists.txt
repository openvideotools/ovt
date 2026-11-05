# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `model` module of libovt-core (chapter 0 §0.3, chapter 06 §6.11). Sources go into ovt_core_objects,
# which ovt_core absorbs.
#
# WI-0.083: the embedded binary schemas (bfbs_resources.cpp includes the ovt_schemas_bfbs.inc fragment that
# cmake/OvtFlatBuffers.cmake generates) and kModelSchemaSha256, computed here at configure time by
# tools/schema_hash.py and written by configure_file below ${OVT_GENERATED_DIR}/model/.

find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
set(_ovt_model_fbs "${PROJECT_SOURCE_DIR}/core/schema/ovt_model.fbs")
# A schema edit re-runs the configure step, so the hash never goes stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ovt_model_fbs}"
                                       "${PROJECT_SOURCE_DIR}/tools/schema_hash.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/schema_hash.py" "${_ovt_model_fbs}"
                OUTPUT_VARIABLE _ovt_hash_out RESULT_VARIABLE _ovt_hash_rc)
string(REGEX MATCH "^[0-9a-f]+" OVT_MODEL_SCHEMA_SHA256 "${_ovt_hash_out}")
string(LENGTH "${OVT_MODEL_SCHEMA_SHA256}" _ovt_hash_len)
if(NOT _ovt_hash_rc EQUAL 0 OR NOT _ovt_hash_len EQUAL 64)
  message(FATAL_ERROR "tools/schema_hash.py failed on ${_ovt_model_fbs}: ${_ovt_hash_out}")
endif()
configure_file(schema_hash.inc.in "${OVT_GENERATED_DIR}/model/schema_hash.inc" @ONLY)
unset(_ovt_model_fbs)
unset(_ovt_hash_out)
unset(_ovt_hash_rc)
unset(_ovt_hash_len)

target_sources(ovt_core_objects PRIVATE bfbs_resources.cpp)
# ovt_schemas orders the build after flatc/embed_bfbs and adds ${OVT_GENERATED_DIR} (SYSTEM) to the
# include path, which is where both .inc fragments live.
target_link_libraries(ovt_core_objects PRIVATE ovt_schemas)
