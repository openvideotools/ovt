# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `cache` module of libovt-core (chapter 0 §0.3, chapter 06 §6.7): the on-disk analysis cache (WI-0.091).
# Sources go into ovt_core_objects, which ovt_core absorbs. This first PR of the WI-0.091 stack carries the
# foundations (atomic files, zstd records, meta.json); cache.cpp (ovt::cache::Cache) and the C API of
# ovt/cache.h (core/src/api/) follow in the next PRs of the stack.

target_sources(ovt_core_objects PRIVATE atomic_file.cpp meta_json.cpp zstd_io.cpp)
# zstd (record compression) and nlohmann_json (meta.json, cache.json, option strings) are implementation
# details: PRIVATE, and inside BUILD_INTERFACE so they never reach an export set (core-objects rule 1, #62).
target_link_libraries(ovt_core_objects PRIVATE "$<BUILD_INTERFACE:${zstd_TARGET}>"
                                               "$<BUILD_INTERFACE:${nlohmann_json_TARGET}>")
