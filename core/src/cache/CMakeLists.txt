# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `cache` module of libovt-core (chapter 0 §0.3, chapter 06 §6.7): ovt::cache::Cache, the on-disk analysis
# cache (WI-0.091). Sources go into ovt_core_objects, which ovt_core absorbs. The C API of ovt/cache.h lives in
# core/src/api/cache_api.cpp.

target_sources(ovt_core_objects PRIVATE atomic_file.cpp cache.cpp meta_json.cpp zstd_io.cpp)
# zstd (record compression) and nlohmann_json (meta.json, cache.json, option strings) are implementation
# details: PRIVATE, and inside BUILD_INTERFACE so they never reach an export set (core-objects rule 1, #62).
target_link_libraries(ovt_core_objects PRIVATE "$<BUILD_INTERFACE:${zstd_TARGET}>"
                                               "$<BUILD_INTERFACE:${nlohmann_json_TARGET}>")
# cache.cpp records OVT_VERSION_FULL (ovt_version_full.h, regenerated on every build) in meta.json.
add_dependencies(ovt_core_objects ovt_version_header)
