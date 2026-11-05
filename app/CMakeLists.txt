# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ovt_studio, the Qt Quick application (chapter 13 §13.2.1; WI-0.019). Desktop builds produce the executable
# `ovt-studio` (Linux) or the bundle `OVT Studio.app` (macOS, §13.5.2); Android builds produce the shared
# library that QtQuickView loads (§13.4). WI-0.019 is only the skeleton: an empty window titled
# OVT_PRODUCT_NAME and `--self-test`. WI-1.450 extends this file, src/main.cpp and qml/Main.qml in place
# (chapter 16 §16.3); it adds QuickControls2, Svg, Graphs and ovt_render to the links of §13.2.1.

# The top-level OvtOptions.cmake already looked for Qt 6 (and turned OVT_BUILD_GUI off without it); Android
# builds come here directly, so find the components again and fail when they are missing.
# The pinned Qt is 6.8.3 (third_party/VERSIONS.md); 6.4 is the oldest the skeleton needs (Ubuntu 24.04's
# packages), so distro builds of the skeleton work. WI-1.450 raises the floor when it needs newer Qt.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_add_executable(ovt_studio MANUAL_FINALIZATION src/main.cpp)
# qt_add_qml_module generates the QML type registration from moc's metatypes.
set_target_properties(ovt_studio PROPERTIES AUTOMOC ON)

# Main.qml sits in app/qml/ (chapter 0 §0.4), but the alias puts it at the module root, so the resource
# path is qrc:/qt/qml/OvtStudio/Main.qml on every Qt version (RESOURCE_PREFIX is explicit because the
# /qt/qml default is policy QTP0001 of Qt 6.5+, and the pinned Qt is 6.8 while distro builds may be older).
set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(
  ovt_studio
  URI
  OvtStudio
  VERSION
  1.0
  RESOURCE_PREFIX
  /qt/qml
  QML_FILES
  qml/Main.qml
  RESOURCES
  resources/icons/ovt-studio.svg)

target_link_libraries(ovt_studio PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick ovt::core ovt::flags)
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
_ovt_studio_describe_qt(ovt_studio) # after finalisation: it adds the QML plugin and cache links

# WI-0.019 acceptance test 1: `ovt-studio --self-test` with QT_QPA_PLATFORM=offscreen exits 0 in < 5 s. It
# needs no display, so it is in the fast lane (no needs_display label); TIMEOUT encodes the 5 s budget.
if(OVT_BUILD_TESTS AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME ovt_studio_self_test COMMAND ovt_studio --self-test)
  set_tests_properties(ovt_studio_self_test PROPERTIES LABELS "fast;gui" TIMEOUT 5 ENVIRONMENT
                                                       "QT_QPA_PLATFORM=offscreen")
endif()
