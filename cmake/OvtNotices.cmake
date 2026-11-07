# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Third-party notices (chapter 15 §15.6, WI-0.603): target ovt_notices runs tools/license_gate.py (which writes
# license_gate_report.json next to license_manifest.json) and then tools/gen_notices.py. The output is
# ${OVT_NOTICES_OUT} (default build/<preset>/THIRD_PARTY_NOTICES.md); `cmake --build . --target ovt_notices`
# twice gives byte-identical files.
#
# Cache variables:
#   OVT_NOTICES_ARTIFACT  artefact id shown in the heading (default "core"; release jobs pass the manifest id)
#   OVT_NOTICES_PATENT    ON for the codec pack: adds the patent statement and writes PATENT_NOTICE.md
#   OVT_NOTICES_REQUIRED  ON in release jobs: `cmake --install` fails if the notices file is missing (default OFF, so a
#                         plain install works without building ovt_notices; WI-0.036 turns it on)
#   OVT_NOTICES_OUT       output file
#
# Licence texts come from the dependencies' install prefixes, never from the network:
#   * ExternalProject dependencies: the LICENSE_FILES of ovt_tp_declare() are staged from the dependency's source
#     directory into ${CMAKE_BINARY_DIR}/notices/licenses/<dep>/ (a step of the ovt_notices target);
#   * vcpkg ports: <vcpkg installed dir>/<triplet>/share/<port>/copyright;
#   * Qt: ${QT_ROOT_DIR}/Licenses.
#
# Install rules (component `notices`, OPTIONAL so a plain `cmake --install` works without building ovt_notices;
# release jobs must build the target first): Linux, AppImage and Flatpak (and the macOS CLI) -> share/doc/ovt/; the macOS
# bundle's Contents/Resources/ is left to the GUI packaging item; Android -> the Gradle assets
# directory android/app/src/main/assets/licenses/ (copied after the build, git-ignored); Python wheel ->
# python/pyproject.toml sets scikit-build-core `wheel.license-files` to the generated file (that file is created
# by the Python packaging work item, not here).

include_guard(GLOBAL)
include(GNUInstallDirs)
include(ExternalProject)
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)

set(OVT_NOTICES_ARTIFACT "core" CACHE STRING "Artefact id for THIRD_PARTY_NOTICES.md")
option(OVT_NOTICES_REQUIRED "Release jobs: cmake --install fails when the notices file was not generated" OFF)
option(OVT_NOTICES_PATENT "Codec pack: add the patent statement and write PATENT_NOTICE.md" OFF)
set(OVT_NOTICES_OUT "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.md" CACHE FILEPATH "Generated notices file")
get_filename_component(_ovt_notices_dir "${OVT_NOTICES_OUT}" DIRECTORY)

set(_ovt_notices_staging "${CMAKE_BINARY_DIR}/notices/licenses")
set(_ovt_notices_report "${CMAKE_BINARY_DIR}/license_gate_report.json")

# Stage the LICENSE_FILES of every ExternalProject dependency declared so far.
set(_ovt_notices_stage_cmds "")
foreach(_dep IN LISTS OVT_TP_DEPS)
  get_property(_files GLOBAL PROPERTY OVT_TP_LICENSE_FILES_tp_${_dep})
  # A script whose ExternalProject or notices component is not named after the VERSIONS.md row says so (onnxruntime.cmake).
  set(_ep tp_${_dep})
  set(_key ${_dep})
  get_property(_ep_set GLOBAL PROPERTY OVT_TP_EP_${_dep} SET)
  if(_ep_set)
    get_property(_ep GLOBAL PROPERTY OVT_TP_EP_${_dep})
    get_property(_key GLOBAL PROPERTY OVT_TP_NOTICE_KEY_${_dep})
  endif()
  if(_files AND TARGET ${_ep})
    externalproject_get_property(${_ep} SOURCE_DIR)
    foreach(_f IN LISTS _files)
      get_filename_component(_name "${_f}" NAME) # staged by base name: gen_notices.py reads <dep>/* without recursion
      list(APPEND _ovt_notices_stage_cmds COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE_DIR}/${_f}"
           "${_ovt_notices_staging}/${_key}/${_name}")
    endforeach()
  endif()
endforeach()
unset(_dep)
unset(_ep)
unset(_ep_set)
unset(_key)
unset(_files)
unset(_f)

set(_ovt_notices_args
    --report
    "${_ovt_notices_report}"
    --versions
    "${CMAKE_SOURCE_DIR}/third_party/VERSIONS.md"
    --licenses-dir
    "${_ovt_notices_staging}"
    --derived
    "${CMAKE_SOURCE_DIR}/tools/derived_files.toml"
    --artifact
    "${OVT_NOTICES_ARTIFACT}"
    --out
    "${OVT_NOTICES_OUT}"
    --version
    "${OVT_VERSION}")
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  list(APPEND _ovt_notices_args --vcpkg-share "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
endif()
if(QT_ROOT_DIR)
  list(APPEND _ovt_notices_args --qt-licenses "${QT_ROOT_DIR}/Licenses")
endif()
if(OVT_NOTICES_PATENT)
  list(APPEND _ovt_notices_args --patent-notice)
endif()

add_custom_target(
  ovt_notices
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ovt_notices_staging}" ${_ovt_notices_stage_cmds}
  COMMAND
    "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/license_gate.py" --policy
    "${CMAKE_SOURCE_DIR}/tools/license_policy.toml" --versions "${CMAKE_SOURCE_DIR}/third_party/VERSIONS.md" --manifest
    "${OVT_LICENSE_MANIFEST}" --report "${_ovt_notices_report}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/gen_notices.py" ${_ovt_notices_args}
  COMMENT "Generating ${OVT_NOTICES_OUT}"
  VERBATIM)
add_dependencies(ovt_notices ovt_license_manifest)
foreach(_dep IN LISTS OVT_TP_DEPS)
  set(_ep tp_${_dep})
  get_property(_ep_set GLOBAL PROPERTY OVT_TP_EP_${_dep} SET)
  if(_ep_set)
    get_property(_ep GLOBAL PROPERTY OVT_TP_EP_${_dep})
  endif()
  if(TARGET ${_ep})
    add_dependencies(ovt_notices ${_ep})
  endif()
endforeach()
unset(_dep)
unset(_ep)
unset(_ep_set)

# ---- install rules ----------------------------------------------------------------------------------
set(_ovt_notices_files "${OVT_NOTICES_OUT}")
if(OVT_NOTICES_PATENT)
  list(APPEND _ovt_notices_files "${_ovt_notices_dir}/PATENT_NOTICE.md")
endif()

if(ANDROID)
  set(_ovt_notices_assets "${CMAKE_SOURCE_DIR}/android/app/src/main/assets/licenses")
  add_custom_command(
    TARGET ovt_notices
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ovt_notices_assets}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_ovt_notices_files} "${_ovt_notices_assets}"
    VERBATIM)
  unset(_ovt_notices_assets)
else()
  # macOS: the bundle's Contents/Resources/ copy belongs to that item (WI-0.060, macOS bundle script): it
  # adds the file to ovt_studio's sources with MACOSX_PACKAGE_LOCATION Resources. A $<TARGET_BUNDLE_CONTENT_DIR>
  # destination would point into the build tree, so it is not used here.
  install(FILES ${_ovt_notices_files} DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/ovt" COMPONENT notices OPTIONAL)
  if(OVT_NOTICES_REQUIRED)
    # Release jobs: a missing notices file is a licence-obligation failure, not something to skip.
    install(
      CODE "foreach(_f IN ITEMS ${_ovt_notices_files})
              if(NOT EXISTS \"\${_f}\")
                message(FATAL_ERROR \"OVT_NOTICES_REQUIRED: \${_f} is missing; build the ovt_notices target first\")
              endif()
            endforeach()"
      COMPONENT notices)
  endif()
endif()
unset(_ovt_notices_files)
