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

add_library(ovt_core SHARED
  src/status.cpp
  src/version.cpp)
add_library(ovt::core ALIAS ovt_core)

target_include_directories(ovt_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${OVT_GENERATED_DIR}")
target_link_libraries(ovt_core PRIVATE ovt::flags)
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

target_compile_definitions(ovt_core PRIVATE
  OVT_VERSION_MAJOR_VALUE=${PROJECT_VERSION_MAJOR}
  OVT_VERSION_MINOR_VALUE=${PROJECT_VERSION_MINOR}
  OVT_VERSION_PATCH_VALUE=${PROJECT_VERSION_PATCH}
  "OVT_VERSION_STRING_VALUE=\"${PROJECT_VERSION}${OVT_VERSION_SUFFIX}\""
  "OVT_BUILD_PROFILE_VALUE=\"${_profile}\""
  "OVT_BUILD_PLATFORM_VALUE=\"${_os}-${_arch}\"")
unset(_os)
unset(_arch)
unset(_profile)

set_target_properties(ovt_core PROPERTIES
  OUTPUT_NAME ovt_core
  EXPORT_NAME core
  VERSION "${OVT_API_VERSION_MAJOR}.${OVT_API_VERSION_MINOR}.${OVT_API_VERSION_PATCH}"
  SOVERSION "${OVT_API_VERSION_MAJOR}")
ovt_set_license(ovt_core "${OVT_CORE_LICENSE}")

# Dependencies from the vcpkg manifest (WI-0.002, chapter 13 §13.3.2). The triplets build them as static
# libraries that ovt_core absorbs, so they stay PRIVATE: a downstream find_package(ovt) never sees them
# (chapter 13 §13.2.6). SPDX ids are the ports' "license" fields.
# SPEC-ASSUMPTION(#8): vcpkg.json carries only the ports some target links; later WIs add theirs.
ovt_find_package(fmt SPDX "MIT" TARGETS fmt::fmt)
ovt_find_package(spdlog SPDX "MIT" TARGETS spdlog::spdlog)
ovt_find_package(flatbuffers SPDX "Apache-2.0" TARGETS flatbuffers::flatbuffers NAMES FlatBuffers)
ovt_find_package(zstd SPDX "BSD-3-Clause OR GPL-2.0-only" TARGETS zstd::libzstd_static zstd::libzstd)
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)
ovt_find_package(hwy SPDX "Apache-2.0" TARGETS hwy::hwy)
ovt_find_package(xxHash SPDX "BSD-2-Clause" TARGETS xxHash::xxhash PKG_CONFIG libxxhash)
target_link_libraries(ovt_core PRIVATE
  ${fmt_TARGET} ${spdlog_TARGET} ${flatbuffers_TARGET} ${zstd_TARGET} ${nlohmann_json_TARGET} ${hwy_TARGET}
  ${xxHash_TARGET})

# One directory per module of chapter 0 §0.3 (NFR-030); each fills its own CMakeLists.txt.
foreach(_module io bitreader container demux syntax index decode analysis model color metrics cache jobs api)
  add_subdirectory(src/${_module})
endforeach()
unset(_module)

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
