# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Top-level build of Open Video Tools (chapter 13 §13.2.3).
# Keep cmake_minimum_required at 3.25: CMakePresets.json version 6 needs exactly that (WI-0.000 notes).
cmake_minimum_required(VERSION 3.25)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OVT_VERSION_RAW)
string(STRIP "${OVT_VERSION_RAW}" OVT_VERSION)

project(OpenVideoTools VERSION ${OVT_VERSION} LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(OvtOptions)        # all options of §13.2.2 and their validation
include(OvtVersion)        # OVT_VERSION_FULL = <VERSION>[<suffix>]+<git sha> (FR-GEN-010)
include(OvtCompilerFlags)  # ovt::flags (§13.2.5)
include(OvtThirdParty)     # dispatches to third_party/*.cmake unless OVT_SYSTEM_* (§13.3)
include(OvtLicenseTags)    # ovt_set_license(<target> <SPDX>) and the ovt_license_manifest target

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
  enable_testing()
  add_subdirectory(tests)
endif()

include(OvtInstall)        # §13.2.6 export set
