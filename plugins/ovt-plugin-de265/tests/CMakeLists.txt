# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Tests of the HEVC analysis plugin's third-party build (WI-0.014), of libde265 patch 0001, the read-only internals
# API (WI-2.162), and of the plugin itself (WI-2.163).
#
# The probe links the patched, LGPL-3.0-or-later libde265, so the test target carries the plugin's licence and
# the ovt_plugin_ name prefix: tools/license_gate.py checks it with the plugin rules (chapter 15 §15.4.2), which
# allow LGPL-3.0 links, and tools/license_policy.toml [targets] lists it.

ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)

add_executable(ovt_plugin_de265_probe_tests test_de265_probe.cpp)
target_link_libraries(ovt_plugin_de265_probe_tests PRIVATE tp::de265 ovt::flags ${Catch2_TARGET})
if(NOT OVT_SYSTEM_DE265)
  # The pinned version of third_party/VERSIONS.md; a system libde265 reports its own.
  target_compile_definitions(ovt_plugin_de265_probe_tests
                             PRIVATE "OVT_DE265_PINNED_VERSION=\"${OVT_TP_libde265_VERSION}\"")
  # Patch 0001 (WI-2.162): only the patched build has de265_internals.h; a system libde265 does not.
  target_sources(ovt_plugin_de265_probe_tests PRIVATE internals_smoke.cpp)
  target_link_libraries(ovt_plugin_de265_probe_tests PRIVATE ovt_test_support)
endif()
ovt_set_license(ovt_plugin_de265_probe_tests "LGPL-3.0-or-later")
ovt_catch_discover_tests(ovt_plugin_de265_probe_tests)

# ---- WI-2.163: the plugin, loaded through the host's plugin registry ----------------------------------------------
# Loads libovt_plugin_de265 with ovt::analysis::PluginRegistry (WI-2.150), like the analysis host does, and checks
# every reconstruction against the stream's decoded picture hash SEI. The host code is LGPL-2.1-or-later; this test
# executable carries the plugin's licence and name prefix (tools/license_policy.toml [targets]).
add_executable(ovt_plugin_de265_smoke_tests test_de265_plugin_smoke.cpp)
target_link_libraries(ovt_plugin_de265_smoke_tests PRIVATE ovt_core_objects tp::ffmpeg_core ovt_schemas
                                                           ovt_test_support ovt::flags ${Catch2_TARGET})
target_compile_definitions(ovt_plugin_de265_smoke_tests
                           PRIVATE "OVT_DE265_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_de265>\"")
add_dependencies(ovt_plugin_de265_smoke_tests ovt_plugin_de265)
ovt_set_license(ovt_plugin_de265_smoke_tests "LGPL-3.0-or-later")
ovt_catch_discover_tests(ovt_plugin_de265_smoke_tests)

if(NOT OVT_SYSTEM_DE265)
  # WI-4.107: the libde265 MV/QP grid oracle that ovt_plugin_ffmpeg_hevc_tests compares the ffmpeg_hevc plugin with.
  add_executable(ovt_plugin_de265_grid_dump de265_grid_dump.cpp)
  target_link_libraries(ovt_plugin_de265_grid_dump PRIVATE tp::de265 ovt::flags)
  ovt_set_license(ovt_plugin_de265_grid_dump "LGPL-3.0-or-later")
endif()

if(NOT OVT_SYSTEM_DE265)
  # Acceptance test 1 of WI-0.014: the build recorded the applied patches for tools/gen_notices.py.
  externalproject_get_property(tp_libde265 INSTALL_DIR)
  add_test(NAME ovt_plugin_de265_patches_recorded
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check_patches_json.py"
                   "${INSTALL_DIR}/share/ovt/patches.json" "${OVT_DE265_PATCH_SERIES}")
  set_tests_properties(ovt_plugin_de265_patches_recorded PROPERTIES LABELS "fast;third_party")
endif()

# ---- WI-2.162 acceptance test 1: DE265_INTERNALS=OFF builds exactly upstream ----------------------------------
# Three extra builds of the pinned tarball with the same compiler and fixed flags: pristine upstream, patched with
# DE265_INTERNALS=OFF and patched with DE265_INTERNALS=ON. -ffile-prefix-map removes the build paths from the
# objects, so the upstream and OFF objects must be byte-identical (deblock.cc carries the one decoding-code change
# of the patch). check_internals_abi.py also compares the `nm` symbol lists. Not built for sanitizer, fuzz or
# cross builds, which do not run this check.
set(_ovt_de265_abi_default ON)
if(OVT_SYSTEM_DE265
   OR CMAKE_CROSSCOMPILING
   OR OVT_ENABLE_SANITIZERS
   OR OVT_ENABLE_FUZZ
   OR NOT CMAKE_NM
   OR NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  set(_ovt_de265_abi_default OFF)
endif()
option(OVT_DE265_ABI_CHECK "Build libde265 three more times to check that patch 0001 changes nothing when OFF"
       ${_ovt_de265_abi_default})
unset(_ovt_de265_abi_default)

if(OVT_DE265_ABI_CHECK)
  ovt_tp_apply_patches(_ovt_abi_patch SERIES "${OVT_DE265_PATCH_SERIES}")
  cmake_path(GET OVT_DE265_PATCH_SERIES PARENT_PATH _ovt_abi_patch_dir)
  file(GLOB _ovt_abi_patch_files CONFIGURE_DEPENDS "${_ovt_abi_patch_dir}/*.patch")
  set(_ovt_abi_dirs "")
  foreach(_variant upstream internals_off internals_on)
    set(_patch_cmd ${_ovt_abi_patch})
    set(_internals OFF)
    if(_variant STREQUAL "upstream")
      set(_patch_cmd "${CMAKE_COMMAND}" -E true) # pristine tarball
    elseif(_variant STREQUAL "internals_on")
      set(_internals ON)
    endif()
    set(_flags "-ffile-prefix-map=<SOURCE_DIR>=libde265 -ffile-prefix-map=<BINARY_DIR>=build")
    set(_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    foreach(_var CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER CMAKE_OSX_ARCHITECTURES
                 CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT)
      if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
        list(APPEND _args "-D${_var}=${${_var}}")
      endif()
    endforeach()
    # The download step reuses the tarball tp_libde265 already fetched into OVT_TP_DOWNLOADS; depending on it
    # keeps two builds from writing the same download at once.
    externalproject_add(
      ovt_de265_abi_${_variant}
      URL ${OVT_TP_libde265_URL}
      URL_HASH SHA256=${OVT_TP_libde265_SHA256}
      DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
      DOWNLOAD_EXTRACT_TIMESTAMP OFF
      PREFIX "${CMAKE_CURRENT_BINARY_DIR}/abi/${_variant}"
      DEPENDS tp_libde265
      PATCH_COMMAND ${_patch_cmd}
      CMAKE_ARGS ${_args}
                 -DCMAKE_BUILD_TYPE=Release
                 "-DCMAKE_C_FLAGS=${_flags}"
                 "-DCMAKE_CXX_FLAGS=${_flags}"
                 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
                 -DBUILD_SHARED_LIBS=OFF
                 -DENABLE_SDL=OFF
                 -DENABLE_DECODER=OFF
                 -DENABLE_ENCODER=OFF
                 -DDE265_INTERNALS=${_internals}
      INSTALL_COMMAND "")
    if(NOT _variant STREQUAL "upstream")
      externalproject_add_stepdependencies(ovt_de265_abi_${_variant} download "${OVT_DE265_PATCH_SERIES}"
                                           ${_ovt_abi_patch_files})
    endif()
    externalproject_get_property(ovt_de265_abi_${_variant} BINARY_DIR)
    list(APPEND _ovt_abi_dirs "${BINARY_DIR}")
  endforeach()
  list(GET _ovt_abi_dirs 0 _abi_upstream)
  list(GET _ovt_abi_dirs 1 _abi_off)
  list(GET _ovt_abi_dirs 2 _abi_on)
  add_test(NAME ovt_plugin_de265_internals_abi
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check_internals_abi.py" --nm "${CMAKE_NM}"
                   --upstream "${_abi_upstream}" --off "${_abi_off}" --on "${_abi_on}")
  set_tests_properties(ovt_plugin_de265_internals_abi PROPERTIES LABELS "fast;third_party")
  unset(_ovt_abi_patch)
  unset(_ovt_abi_patch_dir)
  unset(_ovt_abi_patch_files)
  unset(_ovt_abi_dirs)
  unset(_abi_upstream)
  unset(_abi_off)
  unset(_abi_on)
  unset(_patch_cmd)
  unset(_internals)
  unset(_flags)
  unset(_args)
  unset(_var)
  unset(_variant)
endif()
