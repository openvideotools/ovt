# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# GUI tests (chapter 11 §11.9, WI-1.450): Catch2 tests of the C++ models (app/tests/models) and the Qt Quick Test
# runner of the QML files (app/tests/qml). Both are in the fast lane and need no display (offscreen).

find_package(Qt6 6.4 REQUIRED COMPONENTS QuickTest Test)
# ovt_find_package targets are directory-scoped (core/tests found Catch2 for itself).
ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)

# ---- Models: Settings, Platform, JobsModel, SelectionModel, LogSink, DiagnosticsModel, Timeline index model, Keymap,
# Go to, Project and RecentFiles, without QML ---------------------------------------------------------------------
add_executable(
  ovt_tests_app_models
  models/test_settings.cpp
  models/test_jobs_model.cpp
  models/test_selection_model.cpp
  models/test_log_sink.cpp
  models/test_diagnostics_model.cpp
  models/test_project.cpp
  models/test_series_aggregator.cpp
  models/test_stream_index_model.cpp
  models/test_timeline_viewport.cpp
  models/test_ref_graph_layout.cpp
  models/test_unit_list_model.cpp
  ../src/project.cpp
  models/test_keymap.cpp
  models/test_goto_parser.cpp
  models/test_dock_model.cpp
  models/test_bookmarks_model.cpp
  models/test_sample_streams.cpp
  models/test_picture_controller.cpp
  ../src/picture_controller.cpp
  ../src/settings.cpp
  ../src/keymap.cpp
  ../src/goto_parser.cpp
  ../src/platform/platform.cpp
  ../src/models/jobs_model.cpp
  ../src/models/sample_streams.cpp
  ../src/models/selection_model.cpp
  ../src/models/log_sink.cpp
  ../src/models/diagnostics_model.cpp
  ../src/models/series_aggregator.cpp
  ../src/models/frame_table.cpp
  ../src/models/stream_index_adapter.cpp
  ../src/models/stream_index_model.cpp
  ../src/models/timeline_viewport.cpp
  ../src/models/timeline_geometry.cpp
  ../src/models/ref_graph_layout.cpp
  ../src/models/unit_list_model.cpp
  ../src/models/bookmarks_model.cpp
  ../src/dock/dock_model.cpp)
set_target_properties(ovt_tests_app_models PROPERTIES AUTOMOC ON)
qt_add_resources(
  ovt_tests_app_models
  settings_schema
  PREFIX
  /qt/qml/OvtStudio
  BASE
  "${PROJECT_SOURCE_DIR}/docs/spec/schemas"
  FILES
  "${PROJECT_SOURCE_DIR}/docs/spec/schemas/settings.schema.json"
  "${PROJECT_SOURCE_DIR}/docs/spec/schemas/ovtproj.schema.json")
target_include_directories(ovt_tests_app_models PRIVATE ../src ../src/platform models .)
target_link_libraries(
  ovt_tests_app_models
  PRIVATE Qt6::Core
          Qt6::Gui
          Qt6::Qml
          Qt6::Quick
          Qt6::Test
          ovt::core
          "$<BUILD_INTERFACE:ovt_schemas>"
          ovt::flags
          ${Catch2_TARGET})
target_compile_definitions(ovt_tests_app_models PRIVATE QT_NO_KEYWORDS)
ovt_set_license(ovt_tests_app_models "GPL-3.0-or-later")
_ovt_studio_describe_qt(ovt_tests_app_models)
ovt_catch_discover_tests(ovt_tests_app_models EXTRA_LABELS gui)

# ---- Test IPC (WI-1.465): the command dispatch and the socket server against a small QML scene -----------------
# Only with OVT_ENABLE_TEST_IPC (app/CMakeLists.txt). Its own executable: it needs a QGuiApplication on the offscreen platform, the model tests a QCoreApplication.
if(OVT_ENABLE_TEST_IPC)
  add_executable(ovt_tests_app_testipc testipc/test_test_ipc.cpp ../src/testipc/command_dispatch.cpp
                                       ../src/testipc/test_ipc_server.cpp)
  set_target_properties(ovt_tests_app_testipc PROPERTIES AUTOMOC ON)
  target_include_directories(ovt_tests_app_testipc PRIVATE ../src)
  target_link_libraries(ovt_tests_app_testipc PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test ovt::flags
                                                      ${Catch2_TARGET})
  target_compile_definitions(ovt_tests_app_testipc PRIVATE QT_NO_KEYWORDS)
  ovt_set_license(ovt_tests_app_testipc "GPL-3.0-or-later")
  _ovt_studio_describe_qt(ovt_tests_app_testipc)
  ovt_catch_discover_tests(ovt_tests_app_testipc EXTRA_LABELS gui ENVIRONMENT_MODIFICATION
                           "QT_QPA_PLATFORM=set:offscreen")
endif()

# ---- QML: tst_main.qml runs against the same module as the application ---------------------------------------
qt_add_executable(ovt_tests_app_qml MANUAL_FINALIZATION qml/tst_main.cpp)
ovt_studio_add_qml_module(ovt_tests_app_qml)
target_include_directories(ovt_tests_app_qml PRIVATE .)
target_link_libraries(ovt_tests_app_qml PRIVATE Qt6::QuickTest)
target_compile_definitions(ovt_tests_app_qml PRIVATE "QUICK_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/qml\"")
ovt_set_license(ovt_tests_app_qml "GPL-3.0-or-later")
qt_finalize_executable(ovt_tests_app_qml)
ovt_studio_silence_generated(ovt_tests_app_qml)
_ovt_studio_describe_qt(ovt_tests_app_qml)
add_test(NAME ovt_tests_app_qml COMMAND ovt_tests_app_qml)
set_tests_properties(ovt_tests_app_qml PROPERTIES LABELS "fast;gui" TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ---- PictureHost (WI-1.405): the item on a real RHI through QQuickRenderControl ---------------------------------
# Vulkan on Linux needs an X display for Qt's Vulkan instance (the offscreen platform has none), so the test runs
# under xvfb-run where it exists and skips (Catch2 exit code 4) without a display; macOS renders Metal on the
# offscreen platform. Built only where the item is (OVT_STUDIO_PICTURE_HOST, app/CMakeLists.txt).
if(OVT_STUDIO_PICTURE_HOST)
  add_executable(ovt_tests_app_picture picture/test_picture_host.cpp ../src/picture_host.cpp ../src/picture_host.h
                                       ../src/render_stub.cpp)
  set_target_properties(ovt_tests_app_picture PROPERTIES AUTOMOC ON)
  ovt_studio_add_picture_shaders(ovt_tests_app_picture)
  target_include_directories(ovt_tests_app_picture PRIVATE ../src "${PROJECT_SOURCE_DIR}/render/src")
  target_link_libraries(ovt_tests_app_picture PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Quick ovt::render
                                                      ovt::flags ${Catch2_TARGET})
  target_compile_definitions(ovt_tests_app_picture
                             PRIVATE QT_NO_KEYWORDS "OVT_PATTERNS_DIR=\"${PROJECT_SOURCE_DIR}/tests/corpora/patterns\"")
  ovt_set_license(ovt_tests_app_picture "GPL-3.0-or-later")
  _ovt_studio_describe_qt(ovt_tests_app_picture)
  set(_ovt_picture_cmd "$<TARGET_FILE:ovt_tests_app_picture>")
  if(NOT APPLE)
    find_program(OVT_XVFB_RUN xvfb-run)
    if(OVT_XVFB_RUN)
      set(_ovt_picture_cmd "${OVT_XVFB_RUN}" -a -s "-screen 0 1920x1080x24" "$<TARGET_FILE:ovt_tests_app_picture>")
    endif()
  endif()
  add_test(NAME ovt_tests_app_picture COMMAND ${_ovt_picture_cmd})
  set_tests_properties(ovt_tests_app_picture PROPERTIES LABELS "fast;gui" TIMEOUT 120 SKIP_RETURN_CODE 4)
  unset(_ovt_picture_cmd)
endif()

# Screenshot harness (chapter 14 §14.10.2, WI-0.568): its own directory, because a QML module needs its own output
# directory.
add_subdirectory(screenshots)

# ---- Interaction-script lane (chapter 14 §14.10.3): app/tests/scripts/test_*.py via pytest --------------------
# The IPC client fixtures and the harness tests live here (WI-0.568); WI-1.576 adds the required scripts.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pytest, numpy, skimage" RESULT_VARIABLE _ovt_no_gui_py
                  OUTPUT_QUIET ERROR_QUIET)
  if(_ovt_no_gui_py EQUAL 0)
    add_test(
      NAME gui_scripts_pytest
      COMMAND "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
              --ovt-studio-screenshots "$<TARGET_FILE:ovt_studio_screenshots>" --ovt-studio "$<TARGET_FILE:ovt_studio>"
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(gui_scripts_pytest PROPERTIES LABELS "fast;gui" TIMEOUT 120)
  else()
    message(STATUS "pytest, numpy or scikit-image not found for ${Python3_EXECUTABLE}: app/tests/scripts not "
                   "registered (pip install -r tools/requirements-test.txt)")
  endif()
  unset(_ovt_no_gui_py)
endif()
