# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Dispatcher for third-party dependencies (chapter 13 §13.3). For each dependency it will either include
# third_party/<dep>.cmake (an ExternalProject build into OVT_THIRD_PARTY_PREFIX) or, when the matching
# OVT_SYSTEM_<DEP> option is ON, find the system library. The scripts arrive with WI-0.010 and later;
# until then this file only loads the shared helpers so that every build already goes through it.

include_guard(GLOBAL)

include(OvtThirdPartyCommon) # ExternalProject conventions (§13.3.4), filled by WI-0.010
include(OvtMeson) # Meson wrapper (§13.3.5), WI-0.011

# ---- dav1d (WI-0.011) -------------------------------------------------------------------------------
# tp::dav1d from third_party/dav1d.cmake, or from the system library with OVT_SYSTEM_DAV1D=ON.
# SPEC-ASSUMPTION(#44): while the dav1d row of VERSIONS.md has no SHA-256 (`<sha>`, filled by
# `tools/versions_to_cmake.py --update-hashes`, which needs network access to code.videolan.org) the build
# is skipped with a status message instead of failing every configure.
if(OVT_SYSTEM_DAV1D)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ovt_system_dav1d REQUIRED IMPORTED_TARGET dav1d)
  add_library(tp::dav1d INTERFACE IMPORTED GLOBAL)
  target_link_libraries(tp::dav1d INTERFACE PkgConfig::ovt_system_dav1d)
  ovt_set_dependency_license(
    tp::dav1d
    SOURCE
    system
    SPDX
    "BSD-2-Clause"
    VERSION
    "${ovt_system_dav1d_VERSION}"
    LINKAGE
    shared)
elseif(OVT_TP_dav1d_SHA256)
  include("${CMAKE_CURRENT_LIST_DIR}/../third_party/dav1d.cmake")
else()
  message(STATUS "OVT: dav1d skipped, third_party/VERSIONS.md has no SHA-256 for it yet")
endif()

# ---- libde265 (WI-0.014) ----------------------------------------------------------------------------
# tp::de265, the patched static libde265 of third_party/libde265.cmake (§13.3.6), for the HEVC analysis
# plugin only (LGPL-3.0-or-later, chapter 15 §15.2). OVT_SYSTEM_DE265=ON (the distro preset) takes the
# system library instead, which carries none of the OVT patches.
# SPEC-ASSUMPTION(#114): the system bypass is kept for distro builds, reported unpatched; the plugin
# work of chapter 07 decides whether it may build without the internals API.
if(OVT_BUILD_PLUGIN_DE265)
  if(OVT_SYSTEM_DE265)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ovt_system_de265 REQUIRED IMPORTED_TARGET libde265)
    add_library(tp::de265 INTERFACE IMPORTED GLOBAL)
    target_link_libraries(tp::de265 INTERFACE PkgConfig::ovt_system_de265)
    ovt_set_dependency_license(
      tp::de265
      SOURCE
      system
      SPDX
      "LGPL-3.0-or-later"
      VERSION
      "${ovt_system_de265_VERSION}"
      LINKAGE
      shared)
  else()
    include("${CMAKE_CURRENT_LIST_DIR}/../third_party/libde265.cmake")
  endif()
endif()

# ---- libvmaf (WI-0.015) -------------------------------------------------------------------------------
# tp::vmaf from third_party/libvmaf.cmake, or from the system library with OVT_SYSTEM_LIBVMAF=ON (the distro
# preset). The system library must have been built with its built-in models.
if(OVT_SYSTEM_LIBVMAF)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ovt_system_vmaf REQUIRED IMPORTED_TARGET libvmaf)
  add_library(tp::vmaf INTERFACE IMPORTED GLOBAL)
  target_link_libraries(tp::vmaf INTERFACE PkgConfig::ovt_system_vmaf)
  ovt_set_dependency_license(
    tp::vmaf
    SOURCE
    system
    SPDX
    "BSD-2-Clause-Patent"
    VERSION
    "${ovt_system_vmaf_VERSION}"
    LINKAGE
    shared)
else()
  include("${CMAKE_CURRENT_LIST_DIR}/../third_party/libvmaf.cmake")
endif()

# ---- MoltenVK (WI-0.017) ------------------------------------------------------------------------------
# tp::moltenvk from third_party/moltenvk.cmake: the prebuilt libMoltenVK.dylib (single-architecture slice) that
# ovt_render links directly on macOS (§13.5.6). Only fetched when the renderer is built.
if(APPLE AND OVT_BUILD_RENDER)
  include("${CMAKE_CURRENT_LIST_DIR}/../third_party/moltenvk.cmake")
endif()

# ---- ONNX Runtime prebuilt (WI-0.017) -----------------------------------------------------------------
# tp::onnxruntime from third_party/onnxruntime.cmake when OVT_ONNXRUNTIME_PREBUILT=ON; otherwise the UVQ host
# takes the vcpkg `onnxruntime` port (§13.3.1).
if(OVT_ONNXRUNTIME_PREBUILT)
  include("${CMAKE_CURRENT_LIST_DIR}/../third_party/onnxruntime.cmake")
endif()

# ovt_find_package(<Package> SPDX <expr> TARGETS <target>... [NAMES <config-name>...] [PKG_CONFIG <module>]
#                  [HEADER_ONLY] [OPTIONAL])
#
# Finds one of the vcpkg-manifest dependencies of chapter 13 §13.3.1 (WI-0.002). With OVT_USE_VCPKG=ON the
# package comes from the vcpkg manifest install; with OVT_USE_VCPKG=OFF (Flatpak, distro builds) the same
# find_package(CONFIG) call resolves the system package, and PKG_CONFIG names a pkg-config module used
# when the system package ships no CMake config (xxHash on Debian/Ubuntu). The first target of TARGETS that
# exists is the one OVT links; the call fails when none exists. NAMES lists alternative config-file names
# (FlatBuffers installs FlatBuffersConfig.cmake, which a case-sensitive file system does not match for
# find_package(flatbuffers)).
#
# Each found target is described for the licence manifest (ovt_set_dependency_license) with the
# package's version, so tools/license_gate.py sees vcpkg and system libraries with their SPDX ids.
# Sets <Package>_TARGET in the caller's scope. Call it in the directory whose targets link the package:
# imported targets are visible only in the directory that created them.
function(ovt_find_package package)
  cmake_parse_arguments(PARSE_ARGV 1 arg "HEADER_ONLY;OPTIONAL" "SPDX;PKG_CONFIG" "TARGETS;NAMES")
  if(NOT arg_SPDX OR NOT arg_TARGETS)
    message(FATAL_ERROR "ovt_find_package(${package}): SPDX and TARGETS are required.")
  endif()

  if(arg_NAMES)
    find_package(${package} CONFIG QUIET NAMES ${package} ${arg_NAMES})
  else()
    find_package(${package} CONFIG QUIET)
  endif()
  set(found_target "")
  foreach(t IN LISTS arg_TARGETS)
    if(TARGET ${t})
      set(found_target ${t})
      break()
    endif()
  endforeach()
  set(version "${${package}_VERSION}")
  # Some packages extend the module path from their config file (Catch2 adds the directory of Catch.cmake).
  set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)

  if(NOT found_target AND arg_PKG_CONFIG AND NOT OVT_USE_VCPKG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(_ovt_pc_${package} QUIET IMPORTED_TARGET ${arg_PKG_CONFIG})
      if(TARGET PkgConfig::_ovt_pc_${package})
        list(GET arg_TARGETS 0 found_target)
        add_library(${found_target} INTERFACE IMPORTED)
        target_link_libraries(${found_target} INTERFACE PkgConfig::_ovt_pc_${package})
        set(version "${_ovt_pc_${package}_VERSION}")
      endif()
    endif()
  endif()

  if(NOT found_target)
    if(arg_OPTIONAL)
      set(${package}_TARGET "" PARENT_SCOPE)
      return()
    endif()
    if(OVT_USE_VCPKG)
      set(hint "it is listed in vcpkg.json; check the vcpkg output of the configure step")
    else()
      set(hint "install the system package or configure with OVT_USE_VCPKG=ON")
    endif()
    message(FATAL_ERROR "ovt_find_package: ${package} not found (looked for ${arg_TARGETS}); ${hint}.")
  endif()

  if(OVT_USE_VCPKG)
    set(source vcpkg)
  else()
    set(source system)
  endif()
  if(arg_HEADER_ONLY)
    set(linkage header-only)
  else()
    get_target_property(type ${found_target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
      set(linkage shared)
    elseif(type STREQUAL "STATIC_LIBRARY")
      set(linkage static)
    else()
      set(linkage "")
    endif()
  endif()
  ovt_set_dependency_license(
    ${found_target}
    SOURCE
    ${source}
    SPDX
    "${arg_SPDX}"
    VERSION
    "${version}"
    LINKAGE
    ${linkage})
  # The package's own helper targets and library files behind the found target (Catch2::Catch2 behind
  # Catch2::Catch2WithMain, the pkg-config target and .so path behind xxHash::xxhash) carry the same licence.
  _ovt_describe_package_closure(${found_target} ${source} "${arg_SPDX}" "${version}")
  message(STATUS "OVT dependency: ${package} ${version} (${source}, ${found_target})")
  set(${package}_TARGET ${found_target} PARENT_SCOPE)
endfunction()

# Describes, with the licence of the package, every link item behind <target> that belongs to the same
# package: imported targets in the same namespace (Foo::bar behind Foo::baz), the PkgConfig:: target that
# ovt_find_package created, and plain library paths. Platform libraries (Threads::Threads) are skipped by
# the licence manifest itself. Items already described keep their description.
function(_ovt_describe_package_closure target source spdx version)
  string(REGEX MATCH "^[^:]+::" ns "${target}")
  set(queue ${target})
  set(seen "")
  while(queue)
    list(POP_FRONT queue current)
    if(current IN_LIST seen OR NOT TARGET ${current})
      continue()
    endif()
    list(APPEND seen ${current})
    get_target_property(link_items ${current} INTERFACE_LINK_LIBRARIES)
    if(NOT link_items)
      continue()
    endif()
    foreach(item IN LISTS link_items)
      if(item MATCHES "^\\$<LINK_ONLY:(.+)>$")
        set(item "${CMAKE_MATCH_1}")
      endif()
      if(item MATCHES "^\\$<" OR item MATCHES "^-")
        continue()
      endif()
      set(same_package FALSE)
      if(TARGET ${item})
        if((ns AND item MATCHES "^${ns}") OR item MATCHES "^PkgConfig::_ovt_pc_")
          set(same_package TRUE)
          list(APPEND queue ${item})
        endif()
      elseif(IS_ABSOLUTE "${item}")
        set(same_package TRUE)
      endif()
      if(same_package)
        get_property(described GLOBAL PROPERTY "OVT_DEP_SPDX_${item}" SET)
        if(NOT described)
          ovt_set_dependency_license(${item} SOURCE ${source} SPDX "${spdx}" VERSION "${version}")
        endif()
      endif()
    endforeach()
  endwhile()
endfunction()

# ---- libplacebo (WI-0.015) ----------------------------------------------------------------------------
# tp::placebo from third_party/libplacebo.cmake (after MoltenVK: on macOS it is the Vulkan implementation), or
# from the system library with OVT_SYSTEM_LIBPLACEBO=ON. Only built with the renderer.
if(OVT_BUILD_RENDER)
  if(OVT_SYSTEM_LIBPLACEBO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ovt_system_placebo REQUIRED IMPORTED_TARGET libplacebo)
    add_library(tp::placebo INTERFACE IMPORTED GLOBAL)
    target_link_libraries(tp::placebo INTERFACE PkgConfig::ovt_system_placebo)
    ovt_set_dependency_license(
      tp::placebo
      SOURCE
      system
      SPDX
      "LGPL-2.1-or-later"
      VERSION
      "${ovt_system_placebo_VERSION}"
      LINKAGE
      shared)
  else()
    include("${CMAKE_CURRENT_LIST_DIR}/../third_party/libplacebo.cmake")
  endif()
endif()
