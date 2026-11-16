# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# ovt-plugin-de265 analysis benchmark (chapter 14 §14.9.3 row bench_analysis_de265_gop60_1080p, WI-2.168). It drives
# the plugin through the host's Session (core/src/analysis), so it links the module objects like the unit tests do,
# and only dlopen()s the LGPL-3.0-or-later plugin. ovt_add_benchmark() is defined by tests/perf/CMakeLists.txt
# (WI-0.560b), which adds this directory.

if(TARGET ovt_plugin_de265)
  ovt_add_benchmark(de265 bench_analysis_de265_gop60_1080p LIBRARIES ovt_core_objects ovt_schemas)
  target_include_directories(bench_analysis_de265_gop60_1080p PRIVATE "${PROJECT_SOURCE_DIR}/core/src/analysis")
  target_compile_definitions(bench_analysis_de265_gop60_1080p
                             PRIVATE "OVT_DE265_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_de265>\"")
  add_dependencies(bench_analysis_de265_gop60_1080p ovt_plugin_de265)
endif()
