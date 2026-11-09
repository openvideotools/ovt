# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `metrics` module of libovt-core (chapter 0 §0.3, chapter 9 §9.2.1). WI-3.300a: the NormalizationConfig
# resolver; WI-3.300b: the plugin registry and ovt_metrics_list_descriptors; WI-3.300c adds the MetricJob state
# machine, the JobSpec parser and the ovt/metrics.h job entry points. Sources go into ovt_core_objects, which ovt_core absorbs. Later work items
# add their directories here (align/, kernels/, vmaf/, uvq/, sidecar/, pool/, bd/, export/).

# The NormalizationConfig JSON Schema (core/schema/normalization_config.schema.json) is embedded as a string
# literal, written below ${OVT_GENERATED_DIR} at configure time; an edit of the schema re-runs configure.
set(_ovt_norm_schema "${PROJECT_SOURCE_DIR}/core/schema/normalization_config.schema.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ovt_norm_schema}")
file(READ "${_ovt_norm_schema}" OVT_NORMALIZATION_SCHEMA_TEXT)
configure_file(normalize/normalization_schema.inc.in "${OVT_GENERATED_DIR}/metrics/normalize/normalization_schema.inc"
               @ONLY)
unset(_ovt_norm_schema)
unset(OVT_NORMALIZATION_SCHEMA_TEXT)

target_sources(ovt_core_objects PRIVATE host/c_api.cpp host/plugin_registry.cpp normalize/normalization_config.cpp)
# nlohmann_json (the module headers expose nlohmann::json in their signatures, core-objects rule 1, #62) and
# the JSON Schema validator (WI-3.300 step 5; vcpkg port json-schema-validator, MIT). Both found by
# core/CMakeLists.txt; BUILD_INTERFACE keeps them out of the export set.
target_link_libraries(ovt_core_objects PUBLIC "$<BUILD_INTERFACE:${nlohmann_json_TARGET}>"
                      PRIVATE "$<BUILD_INTERFACE:${nlohmann_json_schema_validator_TARGET}>")
