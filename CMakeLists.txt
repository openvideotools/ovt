# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Top-level build of Open Video Tools (chapter 13 §13.2.3).
# Keep cmake_minimum_required at 3.25: CMakePresets.json version 6 needs exactly that (WI-0.000 notes).
cmake_minimum_required(VERSION 3.25)

# vcpkg toolchain (WI-0.002). The presets name third_party/vcpkg/scripts/buildsystems/vcpkg.cmake; it has to
# be checked before project(), which loads the toolchain. OVT_USE_VCPKG=OFF drops that toolchain so the
# dependencies resolve through plain find_package/pkg-config (chapter 13 §13.2.2).
if(DEFINED OVT_USE_VCPKG AND NOT OVT_USE_VCPKG AND CMAKE_TOOLCHAIN_FILE MATCHES "/vcpkg\\.cmake$")
  set(CMAKE_TOOLCHAIN_FILE "" CACHE FILEPATH "Toolchain file (cleared: OVT_USE_VCPKG=OFF)" FORCE)
elseif(CMAKE_TOOLCHAIN_FILE MATCHES "/vcpkg\\.cmake$" AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(
    FATAL_ERROR
      "vcpkg toolchain not found: ${CMAKE_TOOLCHAIN_FILE}\n"
      "Run `git submodule update --init third_party/vcpkg` (or tools/bootstrap.sh), "
      "or configure with -DOVT_USE_VCPKG=OFF to use system packages.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OVT_VERSION_RAW)
string(STRIP "${OVT_VERSION_RAW}" OVT_VERSION)

project(OpenVideoTools VERSION ${OVT_VERSION} LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(OvtOptions) # all options of §13.2.2 and their validation
include(OvtVersion) # OVT_VERSION_FULL = <VERSION>[<suffix>]+<git sha> (FR-GEN-010)
include(OvtCompilerFlags) # ovt::flags (§13.2.5)
include(OvtLicenseTags) # ovt_set_license(<target> <SPDX>) and the ovt_license_manifest target
include(OvtThirdParty) # dispatches to third_party/*.cmake unless OVT_SYSTEM_* (§13.3)
include(OvtFlatBuffers) # ovt_add_flatbuffers_schema(<target> <fbs>...) (WI-0.003)

# Before any add_subdirectory(): module test directories (core/tests, …) register tests too.
if(OVT_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
if(OVT_BUILD_RENDER)
  add_subdirectory(render)
endif()
add_subdirectory(plugins)
if(OVT_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(OVT_BUILD_GUI)
  add_subdirectory(app)
endif()
if(OVT_BUILD_PYTHON)
  add_subdirectory(python)
endif()
if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()

include(OvtInstall) # §13.2.6 export set
include(OvtNotices) # ovt_notices target and install rules (§15.6, WI-0.603)
