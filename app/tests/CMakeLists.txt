# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# GUI tests (chapter 11 §11.9, WI-1.450): Catch2 tests of the C++ models (app/tests/models) and the Qt Quick Test
# runner of the QML files (app/tests/qml). Both are in the fast lane and need no display (offscreen).

find_package(Qt6 6.4 REQUIRED COMPONENTS QuickTest Test)
# ovt_find_package targets are directory-scoped (core/tests found Catch2 for itself).
ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)

# ---- Models: Settings, Platform, JobsModel, SelectionModel, LogSink, DiagnosticsModel and the Timeline index model,
# without QML ----------------------------------------------------------------------------------------------------
add_executable(
  ovt_tests_app_models
  models/test_settings.cpp
  models/test_jobs_model.cpp
  models/test_selection_model.cpp
  models/test_log_sink.cpp
  models/test_diagnostics_model.cpp
  models/test_series_aggregator.cpp
  models/test_stream_index_model.cpp
  ../src/settings.cpp
  ../src/platform/platform.cpp
  ../src/models/jobs_model.cpp
  ../src/models/selection_model.cpp
  ../src/models/log_sink.cpp
  ../src/models/diagnostics_model.cpp
  ../src/models/series_aggregator.cpp
  ../src/models/frame_table.cpp
  ../src/models/stream_index_adapter.cpp
  ../src/models/stream_index_model.cpp)
set_target_properties(ovt_tests_app_models PROPERTIES AUTOMOC ON)
qt_add_resources(
  ovt_tests_app_models settings_schema PREFIX /qt/qml/OvtStudio BASE "${PROJECT_SOURCE_DIR}/docs/spec/schemas" FILES
  "${PROJECT_SOURCE_DIR}/docs/spec/schemas/settings.schema.json")
target_include_directories(ovt_tests_app_models PRIVATE ../src ../src/platform models)
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

# ---- QML: tst_main.qml runs against the same module as the application ---------------------------------------
qt_add_executable(ovt_tests_app_qml MANUAL_FINALIZATION qml/tst_main.cpp)
ovt_studio_add_qml_module(ovt_tests_app_qml)
target_link_libraries(ovt_tests_app_qml PRIVATE Qt6::QuickTest)
target_compile_definitions(ovt_tests_app_qml PRIVATE "QUICK_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/qml\"")
ovt_set_license(ovt_tests_app_qml "GPL-3.0-or-later")
qt_finalize_executable(ovt_tests_app_qml)
ovt_studio_silence_generated(ovt_tests_app_qml)
_ovt_studio_describe_qt(ovt_tests_app_qml)
add_test(NAME ovt_tests_app_qml COMMAND ovt_tests_app_qml)
set_tests_properties(ovt_tests_app_qml PROPERTIES LABELS "fast;gui" TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
