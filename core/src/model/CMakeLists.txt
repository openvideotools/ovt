# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `model` module of libovt-core (chapter 0 §0.3, chapter 06 §6.11). Sources go into ovt_core_objects,
# which ovt_core absorbs.
#
# WI-0.083: the embedded binary schemas (bfbs_resources.cpp includes the ovt_schemas_bfbs.inc fragment that
# cmake/OvtFlatBuffers.cmake generates) and kModelSchemaSha256, computed here at configure time by
# tools/schema_hash.py and written by configure_file below ${OVT_GENERATED_DIR}/model/ (all three schemas since WI-0.091).

find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
# One hash per schema (WI-0.091: meta.json records all three, §6.7.2). A schema edit re-runs the configure
# step, so the hashes never go stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/schema_hash.py")
foreach(_ovt_schema model container metrics)
  set(_ovt_fbs "${PROJECT_SOURCE_DIR}/core/schema/ovt_${_ovt_schema}.fbs")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ovt_fbs}")
  execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/schema_hash.py" "${_ovt_fbs}"
                  OUTPUT_VARIABLE _ovt_hash_out RESULT_VARIABLE _ovt_hash_rc)
  string(TOUPPER "${_ovt_schema}" _ovt_schema_upper)
  string(REGEX MATCH "^[0-9a-f]+" OVT_${_ovt_schema_upper}_SCHEMA_SHA256 "${_ovt_hash_out}")
  string(LENGTH "${OVT_${_ovt_schema_upper}_SCHEMA_SHA256}" _ovt_hash_len)
  if(NOT _ovt_hash_rc EQUAL 0 OR NOT _ovt_hash_len EQUAL 64)
    message(FATAL_ERROR "tools/schema_hash.py failed on ${_ovt_fbs}: ${_ovt_hash_out}")
  endif()
endforeach()
configure_file(schema_hash.inc.in "${OVT_GENERATED_DIR}/model/schema_hash.inc" @ONLY)
unset(_ovt_schema)
unset(_ovt_fbs)
unset(_ovt_schema_upper)
unset(_ovt_hash_out)
unset(_ovt_hash_rc)
unset(_ovt_hash_len)

# WI-0.084: the model helpers of ovt/cxx/model/{grid,tree,units,labels}.hpp.
# WI-0.087: the PictureStore and PlaneBlob of ovt/cxx/model/{picture_store,plane}.hpp.
target_sources(ovt_core_objects PRIVATE bfbs_resources.cpp grid.cpp labels.cpp picture_store.cpp tree.cpp units.cpp)
# ovt_schemas orders the build after flatc/embed_bfbs and adds ${OVT_GENERATED_DIR} (SYSTEM) to the
# include path, which is where both .inc fragments and the generated headers live. PUBLIC since WI-0.084:
# the helper headers include ovt_model_generated.h and the flatbuffers headers, so every target that links
# the module objects needs both (core-objects rule 1, #62). BUILD_INTERFACE keeps it out of export sets.
target_link_libraries(ovt_core_objects PUBLIC "$<BUILD_INTERFACE:ovt_schemas>")
