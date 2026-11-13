# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Screenshot harness (chapter 14 §14.10.2, WI-0.568).
# `ovt_studio_screenshots --scene <name> --theme light|dark --out <dir>` renders a scene of the OvtStudio module
# offscreen. The harness is a test tool, not a test: the pytest lane (app/tests/CMakeLists.txt) and WI-1.576 run it.
qt_add_executable(ovt_studio_screenshots MANUAL_FINALIZATION ovt_studio_screenshots.cpp)
ovt_studio_add_qml_module(ovt_studio_screenshots)
target_compile_definitions(ovt_studio_screenshots PRIVATE "OVT_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"" QT_NO_KEYWORDS)
ovt_set_license(ovt_studio_screenshots "GPL-3.0-or-later")
qt_finalize_executable(ovt_studio_screenshots)
ovt_studio_silence_generated(ovt_studio_screenshots)
_ovt_studio_describe_qt(ovt_studio_screenshots)
