# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ovt_studio, the Qt Quick application (chapter 13 §13.2.1; WI-0.019). Desktop builds produce the executable
# `ovt-studio` (Linux) or the bundle `OVT Studio.app` (macOS, §13.5.2); Android builds produce the shared
# library that QtQuickView loads (§13.4). WI-0.019 is only the skeleton: an empty window titled
# OVT_PRODUCT_NAME and `--self-test`. WI-1.450 extends this file, src/main.cpp and qml/Main.qml in place
# (chapter 16 §16.3): the application shell, Platform, Settings and QuickControls2 (Svg, Graphs and ovt_render follow).

# The top-level OvtOptions.cmake already looked for Qt 6 (and turned OVT_BUILD_GUI off without it); Android
# builds come here directly, so find the components again and fail when they are missing.
# The pinned Qt is 6.8.3 (third_party/VERSIONS.md); 6.4 is the oldest the shell needs (Ubuntu 24.04's
# packages), so distro builds work. WI-1.450 adds QuickControls2; Svg, Graphs and ovt_render follow with the
# work items that use them (§13.2.1).
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

# The QML module "OvtStudio": the QML files, the C++ types they use (Platform, Settings) and the settings schema.
# A function so that the QML test executable (app/tests/qml) builds the same module under the same URI. The QML
# files sit in app/qml/ (chapter 0 §0.4), but the aliases put them at the module root, so Main.qml is
# qrc:/qt/qml/OvtStudio/Main.qml on every Qt version (RESOURCE_PREFIX is explicit because the /qt/qml default is
# policy QTP0001 of Qt 6.5+, and the pinned Qt is 6.8 while distro builds may be older). OvtStyle.qml is a
# singleton; the schema is the one spec file (docs/spec/schemas) that the program embeds.
function(ovt_studio_add_qml_module target)
  # Absolute paths: the function is also called from app/tests/qml.
  set(app_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  set(qml_files
      "${app_dir}/qml/Main.qml" "${app_dir}/qml/Actions.qml" "${app_dir}/qml/OvtMenu.qml"
      "${app_dir}/qml/OvtMenuBar.qml" "${app_dir}/qml/controls/OvtStyle.qml" "${app_dir}/qml/JobsPopover.qml"
      "${app_dir}/qml/panels/DiagnosticsPanel.qml")
  foreach(file IN LISTS qml_files)
    get_filename_component(alias "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
  endforeach()
  set_source_files_properties("${app_dir}/qml/controls/OvtStyle.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
  # Qt 6.8 requires an alias on every absolute path used as a resource; the icon keeps its path under the module.
  set_source_files_properties("${app_dir}/resources/icons/ovt-studio.svg" PROPERTIES QT_RESOURCE_ALIAS
                                                                                     resources/icons/ovt-studio.svg)
  set(schema "${PROJECT_SOURCE_DIR}/docs/spec/schemas/settings.schema.json")
  set_source_files_properties("${schema}" PROPERTIES QT_RESOURCE_ALIAS settings.schema.json)
  # qt_add_qml_module generates the QML type registration from moc's metatypes.
  set_target_properties(${target} PROPERTIES AUTOMOC ON)
  qt_add_qml_module(
    ${target}
    URI
    OvtStudio
    VERSION
    1.0
    RESOURCE_PREFIX
    /qt/qml
    QML_FILES
    ${qml_files}
    SOURCES
    "${app_dir}/src/platform/platform.h"
    "${app_dir}/src/platform/platform.cpp"
    "${app_dir}/src/settings.h"
    "${app_dir}/src/settings.cpp"
    "${app_dir}/src/models/diagnostics_model.h"
    "${app_dir}/src/models/diagnostics_model.cpp"
    "${app_dir}/src/models/jobs_model.h"
    "${app_dir}/src/models/jobs_model.cpp"
    "${app_dir}/src/models/log_sink.h"
    "${app_dir}/src/models/log_sink.cpp"
    "${app_dir}/src/models/selection_model.h"
    "${app_dir}/src/models/selection_model.cpp"
    "${app_dir}/src/models/series_aggregator.h"
    "${app_dir}/src/models/series_aggregator.cpp"
    "${app_dir}/src/models/frame_table.h"
    "${app_dir}/src/models/frame_table.cpp"
    "${app_dir}/src/models/stream_index_adapter.h"
    "${app_dir}/src/models/stream_index_adapter.cpp"
    "${app_dir}/src/models/stream_index_model.h"
    "${app_dir}/src/models/stream_index_model.cpp"
    RESOURCES
    "${app_dir}/resources/icons/ovt-studio.svg"
    "${schema}")
  # src/platform too: the generated type registration includes the headers by file name.
  target_include_directories(${target} PRIVATE "${app_dir}/src" "${app_dir}/src/platform" "${app_dir}/src/models")
  # ovt::core for ovt/jobs.h and ovt/log.h (the models use the exported C API only, see jobs_model.h).
  # ovt_schemas: ovt_model_generated.h and the FlatBuffers runtime for the index adapter (WI-1.455a; build tree only,
  # as for the core module objects).
  target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 ovt::core
                                          "$<BUILD_INTERFACE:ovt_schemas>" ovt::flags)
  # No QSettings anywhere in app/ (D8) and no Qt keyword macros (the headers use Q_SIGNALS).
  target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS)
endfunction()

# Generated code is not ours to fix: Qt's QML cache compiler (AOT) and type registration output trips -Wshadow and
# friends on some Qt versions, which would fail the -Werror builds. Call after qt_finalize_executable().
function(ovt_studio_silence_generated target)
  get_target_property(sources ${target} SOURCES)
  foreach(source IN LISTS sources)
    if(source MATCHES "/\\.rcc/|qmltyperegistrations|/qrc_|_autogen/")
      set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-w")
    endif()
  endforeach()
endfunction()

qt_add_executable(ovt_studio MANUAL_FINALIZATION src/main.cpp)
ovt_studio_add_qml_module(ovt_studio)

target_link_libraries(ovt_studio PRIVATE ovt::core)
target_include_directories(ovt_studio PRIVATE "${OVT_GENERATED_DIR}")
add_dependencies(ovt_studio ovt_version_header)
# Every user-visible product name derives from OVT_PRODUCT_NAME (chapter 15 §15.7.4).
target_compile_definitions(ovt_studio PRIVATE "OVT_PRODUCT_NAME_VALUE=\"${OVT_PRODUCT_NAME}\"" QT_NO_KEYWORDS)
ovt_set_license(ovt_studio "GPL-3.0-or-later")

# Qt comes from tools/install_qt.py (aqtinstall, pinned in third_party/VERSIONS.md) or, for distro and
# Flatpak builds, from the system; the licence manifest records it either way (chapter 15 §15.4). Walks the
# link closure of the Qt targets above: Qt's own targets (Qt6::*, the versionless Qt::* and Qt's Wrap*
# helper targets) carry Qt's licence; the system OpenGL targets that QtGui pulls in on Linux are libglvnd /
# Mesa (MIT). Items already described keep their description.
function(_ovt_studio_describe_qt)
  if(OVT_USE_VCPKG)
    set(source third_party)
  else()
    set(source system)
  endif()
  set(queue ${ARGN})
  set(seen "")
  while(queue)
    list(POP_FRONT queue current)
    if(current IN_LIST seen OR NOT TARGET ${current})
      continue()
    endif()
    list(APPEND seen ${current})
    get_target_property(imported ${current} IMPORTED)
    if(current MATCHES "^(Qt6?|Wrap[A-Za-z0-9]*)::")
      set(spdx "LGPL-3.0-only")
      set(version "${Qt6_VERSION}")
    elseif(current MATCHES "^OpenGL::")
      set(spdx "MIT")
      set(version "")
    elseif(imported)
      continue() # another package: described where it is found
    else()
      set(spdx "") # an OVT or Qt-generated target (ovt_studio, its qmlcache objects): walk through it
    endif()
    if(spdx)
      get_property(described GLOBAL PROPERTY "OVT_DEP_SPDX_${current}" SET)
      if(NOT described)
        ovt_set_dependency_license(${current} SOURCE ${source} SPDX "${spdx}" VERSION "${version}")
      endif()
    endif()
    set(link_items "")
    foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      if(imported AND property STREQUAL "LINK_LIBRARIES")
        continue()
      endif()
      get_target_property(items ${current} ${property})
      if(items)
        list(APPEND link_items ${items})
      endif()
    endforeach()
    foreach(item IN LISTS link_items)
      if(item MATCHES "^\\$<LINK_ONLY:(.+)>$")
        set(item "${CMAKE_MATCH_1}")
      endif()
      if(TARGET ${item})
        list(APPEND queue ${item})
      endif()
    endforeach()
  endwhile()
endfunction()

if(APPLE)
  # OVT Studio.app/Contents/MacOS/OVT Studio (§13.5.2).
  set_target_properties(
    ovt_studio
    PROPERTIES OUTPUT_NAME "${OVT_PRODUCT_NAME}"
               MACOSX_BUNDLE TRUE
               MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
               MACOSX_BUNDLE_BUNDLE_NAME "${OVT_PRODUCT_NAME}"
               MACOSX_BUNDLE_GUI_IDENTIFIER "org.openvideotools.studio"
               MACOSX_BUNDLE_BUNDLE_VERSION "${OVT_VERSION}"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${OVT_VERSION}")
elseif(ANDROID)
  # Qt names the library libovt_studio_<ABI>.so; androiddeployqt looks for exactly that name.
  # SPEC-ASSUMPTION(#71): this is the `libovt_studio.so` of §13.2.1 and WI-0.019 acceptance test 3.
else()
  set_target_properties(ovt_studio PROPERTIES OUTPUT_NAME "ovt-studio")
endif()

qt_finalize_executable(ovt_studio)
ovt_studio_silence_generated(ovt_studio)
_ovt_studio_describe_qt(ovt_studio) # after finalisation: it adds the QML plugin and cache links

# WI-0.019 acceptance test 1: `ovt-studio --self-test` with QT_QPA_PLATFORM=offscreen exits 0 in < 5 s on Linux.
# It needs no display, so it is in the fast lane (no needs_display label). The 5 s budget stays on non-Apple
# platforms; on Apple platforms the loaded Mac Mini runners made it flaky, so there the in-app watchdog is 25 s and
# the CTest TIMEOUT 30 s. RUN_SERIAL keeps it from competing with other tests everywhere.
if(APPLE)
  set(_ovt_self_test_timeout 30)
  target_compile_definitions(ovt_studio PRIVATE OVT_SELF_TEST_WATCHDOG_MS=25000)
else()
  set(_ovt_self_test_timeout 5)
endif()
if(OVT_BUILD_TESTS AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME ovt_studio_self_test COMMAND ovt_studio --self-test)
  # OVT_CONFIG_DIR keeps the test away from the user's own settings.json (§11.2.5).
  set_tests_properties(
    ovt_studio_self_test
    PROPERTIES LABELS "fast;gui" TIMEOUT ${_ovt_self_test_timeout} RUN_SERIAL TRUE ENVIRONMENT
               "QT_QPA_PLATFORM=offscreen;OVT_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/self_test_config")
  # WI-1.450 acceptance test 1: --version prints the versions and exits 0, without a display (no QT_QPA_PLATFORM).
  add_test(NAME ovt_studio_version COMMAND ovt_studio --version)
  set_tests_properties(ovt_studio_version PROPERTIES LABELS "fast;gui" TIMEOUT 5 PASS_REGULAR_EXPRESSION
                                                     "core [0-9][^\n]*\nQt [0-9]")
  add_subdirectory(tests)
endif()
