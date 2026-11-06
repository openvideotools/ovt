# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# ONNX Runtime, prebuilt fallback (chapter 13 §13.3.1, WI-0.017). Used with OVT_ONNXRUNTIME_PREBUILT=ON when
# the vcpkg `onnxruntime` port is unavailable on a platform. Produces the imported target tp::onnxruntime: the
# official release archive (CPU execution provider) extracted to <prefix>/onnxruntime, with the C and C++
# headers under include/ and libonnxruntime (.so or .dylib) under lib/. Not available for Android (OVT_ENABLE_UVQ
# is OFF there).
#
# The archive differs per platform, so VERSIONS.md has one row per archive: `ONNX Runtime` (Linux x64) and
# `ONNX Runtime linux-aarch64`, `ONNX Runtime osx-arm64`, `ONNX Runtime osx-x86_64`. The row is chosen from
# CMAKE_OSX_ARCHITECTURES (else CMAKE_SYSTEM_PROCESSOR). The pins come only from those rows.

include_guard(GLOBAL)
include(OvtThirdPartyCommon)

if(CMAKE_OSX_ARCHITECTURES)
  set(_ovt_ort_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
  set(_ovt_ort_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
list(LENGTH _ovt_ort_arch _ovt_ort_arch_count)
if(_ovt_ort_arch_count GREATER 1)
  message(FATAL_ERROR "third_party/onnxruntime.cmake: the prebuilt archives are per architecture; configure one "
                      "architecture (CMAKE_OSX_ARCHITECTURES='${_ovt_ort_arch}') or use the vcpkg port.")
endif()

if(ANDROID)
  message(FATAL_ERROR "third_party/onnxruntime.cmake: no prebuilt ONNX Runtime for Android (UVQ is off there).")
elseif(APPLE AND _ovt_ort_arch STREQUAL "arm64")
  set(_ovt_ort_dep onnx_runtime_osx_arm64)
  set(_ovt_ort_url ${OVT_TP_onnx_runtime_osx_arm64_URL})
  set(_ovt_ort_sha ${OVT_TP_onnx_runtime_osx_arm64_SHA256})
elseif(APPLE AND _ovt_ort_arch STREQUAL "x86_64")
  set(_ovt_ort_dep onnx_runtime_osx_x86_64)
  set(_ovt_ort_url ${OVT_TP_onnx_runtime_osx_x86_64_URL})
  set(_ovt_ort_sha ${OVT_TP_onnx_runtime_osx_x86_64_SHA256})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _ovt_ort_arch MATCHES "^(x86_64|AMD64)$")
  set(_ovt_ort_dep onnx_runtime)
  set(_ovt_ort_url ${OVT_TP_onnx_runtime_URL})
  set(_ovt_ort_sha ${OVT_TP_onnx_runtime_SHA256})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _ovt_ort_arch MATCHES "^(aarch64|arm64)$")
  set(_ovt_ort_dep onnx_runtime_linux_aarch64)
  set(_ovt_ort_url ${OVT_TP_onnx_runtime_linux_aarch64_URL})
  set(_ovt_ort_sha ${OVT_TP_onnx_runtime_linux_aarch64_SHA256})
else()
  message(FATAL_ERROR "third_party/onnxruntime.cmake: no prebuilt ONNX Runtime for ${CMAKE_SYSTEM_NAME} "
                      "${_ovt_ort_arch}.")
endif()

ovt_tp_declare(${_ovt_ort_dep} LICENSE_FILES LICENSE)
set(_ovt_ort_licence "${OVT_TP_${_ovt_ort_dep}_LICENSE}")
set(_ovt_ort_version "${OVT_TP_${_ovt_ort_dep}_VERSION}")

set(_ovt_ort_prefix "${OVT_THIRD_PARTY_PREFIX}/onnxruntime")
if(APPLE)
  set(_ovt_ort_lib "${_ovt_ort_prefix}/lib/libonnxruntime.dylib")
else()
  set(_ovt_ort_lib "${_ovt_ort_prefix}/lib/libonnxruntime.so")
endif()

# The archive is a ready layout (include/, lib/, LICENSE), so it is extracted straight into the prefix: no
# configure, build or install step (its stamps live in the sibling onnxruntime_ep directory). The libonnxruntime symlinks of the archive stay symlinks.
externalproject_add(
  tp_onnxruntime
  URL ${_ovt_ort_url}
  URL_HASH SHA256=${_ovt_ort_sha}
  DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
  DOWNLOAD_EXTRACT_TIMESTAMP OFF
  PREFIX ${_ovt_ort_prefix}_ep
  SOURCE_DIR ${_ovt_ort_prefix}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${_ovt_ort_lib}")

# INTERFACE_INCLUDE_DIRECTORIES must exist at generate time, before the build extracts anything.
file(MAKE_DIRECTORY "${_ovt_ort_prefix}/include")
add_library(tp::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(tp::onnxruntime PROPERTIES IMPORTED_LOCATION "${_ovt_ort_lib}" INTERFACE_INCLUDE_DIRECTORIES
                                                                                     "${_ovt_ort_prefix}/include")
if(APPLE)
  # The dylib's install name is @rpath/...; CMake cannot read it before the archive is extracted (see moltenvk.cmake).
  set_property(TARGET tp::onnxruntime PROPERTY INTERFACE_LINK_OPTIONS "LINKER:-rpath,${_ovt_ort_prefix}/lib")
endif()
add_dependencies(tp::onnxruntime tp_onnxruntime)
ovt_set_dependency_license(
  tp::onnxruntime
  SOURCE
  third_party
  SPDX
  "${_ovt_ort_licence}"
  VERSION
  "${_ovt_ort_version}"
  LINKAGE
  shared)

unset(_ovt_ort_arch)
unset(_ovt_ort_arch_count)
unset(_ovt_ort_dep)
unset(_ovt_ort_url)
unset(_ovt_ort_sha)
unset(_ovt_ort_licence)
unset(_ovt_ort_version)
unset(_ovt_ort_prefix)
unset(_ovt_ort_lib)
