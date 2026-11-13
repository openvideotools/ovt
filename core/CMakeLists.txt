# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libovt-core (chapter 13 §13.2.1). In WI-0.000 it holds only the version and status entry points; WI-0.002
# links the vcpkg-manifest libraries it will use (chapter 13 §13.3.1).

# The shared library's SONAME follows the C API major version (chapter 12 §12.3.2), read from the header
# so that ovt/version.h stays the single source.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/ovt/version.h" _ovt_api_lines
     REGEX "^#define OVT_API_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_part MAJOR MINOR PATCH)
  string(REGEX MATCH "OVT_API_VERSION_${_part} ([0-9]+)" _m "${_ovt_api_lines}")
  set(OVT_API_VERSION_${_part} "${CMAKE_MATCH_1}")
endforeach()
unset(_ovt_api_lines)
unset(_part)
unset(_m)

add_library(ovt_core SHARED src/status.cpp src/version.cpp)
add_library(ovt::core ALIAS ovt_core)

target_include_directories(
  ovt_core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>" PRIVATE "${OVT_GENERATED_DIR}")
target_link_libraries(ovt_core PRIVATE ovt::flags)
# A sanitized libovt_core calls into the sanitizer runtime, which Clang on Linux links only into executables, not
# into shared libraries. An installed consumer must therefore link with the same -fsanitize flags, so the exported
# target carries them as a usage requirement (OVT_INSTALL_SANITIZER_LINK_FLAGS, cmake/OvtTesting.cmake). In-tree
# targets get the plain flags from ovt::flags already.
if(OVT_INSTALL_SANITIZER_LINK_FLAGS)
  target_link_options(ovt_core INTERFACE "$<INSTALL_INTERFACE:${OVT_INSTALL_SANITIZER_LINK_FLAGS}>")
endif()
add_dependencies(ovt_core ovt_version_header)

# Platform id in the preset spelling (linux-x64, macos-arm64, macos-x86_64, android-arm64, ...).
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_os "macos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(_os "android")
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _os)
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(GET CMAKE_OSX_ARCHITECTURES 0 _arch)
elseif(ANDROID AND CMAKE_ANDROID_ARCH_ABI)
  set(_arch "${CMAKE_ANDROID_ARCH_ABI}")
else()
  set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_arch}" _arch)
if(_arch MATCHES "^(aarch64|arm64|arm64-v8a)$")
  set(_arch "arm64")
elseif(_arch MATCHES "^(x86_64|amd64)$")
  if(_os STREQUAL "linux")
    set(_arch "x64")
  else()
    set(_arch "x86_64")
  endif()
endif()
if(CMAKE_BUILD_TYPE)
  string(TOLOWER "${CMAKE_BUILD_TYPE}" _profile)
else()
  set(_profile "default")
endif()

target_compile_definitions(
  ovt_core
  PRIVATE OVT_VERSION_MAJOR_VALUE=${PROJECT_VERSION_MAJOR}
          OVT_VERSION_MINOR_VALUE=${PROJECT_VERSION_MINOR}
          OVT_VERSION_PATCH_VALUE=${PROJECT_VERSION_PATCH}
          "OVT_VERSION_STRING_VALUE=\"${PROJECT_VERSION}${OVT_VERSION_SUFFIX}\""
          "OVT_BUILD_PROFILE_VALUE=\"${_profile}\""
          "OVT_BUILD_PLATFORM_VALUE=\"${_os}-${_arch}\"")
unset(_os)
unset(_arch)
unset(_profile)

set_target_properties(
  ovt_core
  PROPERTIES OUTPUT_NAME ovt_core EXPORT_NAME core
             VERSION "${OVT_API_VERSION_MAJOR}.${OVT_API_VERSION_MINOR}.${OVT_API_VERSION_PATCH}"
             SOVERSION "${OVT_API_VERSION_MAJOR}")
ovt_set_license(ovt_core "${OVT_CORE_LICENSE}")

# Dependencies from the vcpkg manifest (WI-0.002, chapter 13 §13.3.2). The triplets build them as static
# libraries that ovt_core absorbs, so they stay PRIVATE: a downstream find_package(ovt) never sees them
# (chapter 13 §13.2.6). SPDX ids are the ports' "license" fields.
# vcpkg.json carries only the ports some target links; later WIs add theirs (chapter 13 §13.3.2).
ovt_find_package(fmt SPDX "MIT" TARGETS fmt::fmt)
ovt_find_package(spdlog SPDX "MIT" TARGETS spdlog::spdlog)
ovt_find_package(flatbuffers SPDX "Apache-2.0" TARGETS flatbuffers::flatbuffers NAMES FlatBuffers)
# The vcpkg triplets build zstd as a PIC static library that ovt_core absorbs. A distribution's static libzstd is
# usually not PIC and cannot go into a shared library, so a system build (OVT_USE_VCPKG=OFF) links the shared one.
if(OVT_USE_VCPKG)
  set(_ovt_zstd_targets zstd::libzstd_static zstd::libzstd)
else()
  set(_ovt_zstd_targets zstd::libzstd zstd::libzstd_shared zstd::libzstd_static)
endif()
ovt_find_package(zstd SPDX "BSD-3-Clause OR GPL-2.0-only" TARGETS ${_ovt_zstd_targets})
unset(_ovt_zstd_targets)
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)
# JSON Schema validation of NormalizationConfig (chapter 9 §9.2.4, WI-3.300); 2.4.0 exports ::validator, older
# releases the bare name.
ovt_find_package(nlohmann_json_schema_validator SPDX "MIT" TARGETS nlohmann_json_schema_validator::validator
                 nlohmann_json_schema_validator)
ovt_find_package(hwy SPDX "Apache-2.0" TARGETS hwy::hwy)
ovt_find_package(xxHash SPDX "BSD-2-Clause" TARGETS xxHash::xxhash PKG_CONFIG libxxhash)
# $<BUILD_INTERFACE:...> keeps them out of the exported ovt-targets.cmake even when a system build
# (OVT_USE_VCPKG=OFF) links them as shared libraries (WI-0.008).
foreach(_dep ${fmt_TARGET} ${spdlog_TARGET} ${flatbuffers_TARGET} ${zstd_TARGET} ${nlohmann_json_TARGET} ${hwy_TARGET}
             ${xxHash_TARGET} ${nlohmann_json_schema_validator_TARGET})
  target_link_libraries(ovt_core PRIVATE "$<BUILD_INTERFACE:${_dep}>")
endforeach()
unset(_dep)

# Module sources (WI-1.100 onwards) compile once into the OBJECT library ovt_core_objects, which ovt_core
# absorbs. The unit tests of core/tests/<module>/ link ovt_core_objects directly: ovt_core is built with
# hidden visibility and exports only the C API (chapter 12 §12.3.2), so the C++ classes of the modules are
# not reachable through the shared library. SPEC-ASSUMPTION(#62): this layout.
add_library(ovt_core_objects OBJECT)
target_include_directories(ovt_core_objects PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                                                   "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
# size_t/off_t are 64-bit everywhere (chapter 7 WI-1.100 notes).
target_compile_definitions(ovt_core_objects PUBLIC "$<$<PLATFORM_ID:Linux>:_FILE_OFFSET_BITS=64>")
find_package(Threads REQUIRED)
target_link_libraries(ovt_core_objects PUBLIC Threads::Threads PRIVATE ovt::flags)
# The module headers use fmt (ovt/cxx/log.hpp, WI-1.512), so module code and the tests that link the objects
# need it: PUBLIC, and inside BUILD_INTERFACE so it never reaches an export set (core-objects rule 1, #62).
target_link_libraries(ovt_core_objects PUBLIC "$<BUILD_INTERFACE:${fmt_TARGET}>")
ovt_set_license(ovt_core_objects "${OVT_CORE_LICENSE}")
target_link_libraries(ovt_core PRIVATE ovt_core_objects)

# FlatBuffers schemas and their generated code: target ovt_schemas (WI-0.003).
add_subdirectory(schema)

# One directory per module of chapter 0 §0.3 (NFR-030); each fills its own CMakeLists.txt.
foreach(
  _module
  io
  log
  bitreader
  container
  demux
  syntax
  index
  decode
  analysis
  model
  color
  metrics
  cache
  jobs
  api)
  add_subdirectory(src/${_module})
endforeach()
unset(_module)

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
