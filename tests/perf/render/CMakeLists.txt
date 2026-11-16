# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Render benchmarks (chapter 10 §10.9.3, chapter 14 §14.9.3 rows bench_render_*). WI-1.415 adds the video path
# (bench_render_video_4k, bench_render_first_frame); WI-2.438 adds the overlay benchmarks to this directory.
# ovt_add_benchmark() is defined by tests/perf/CMakeLists.txt (WI-0.560b), which adds this directory when the render
# library is built.

ovt_add_benchmark(render bench_render_video LIBRARIES ovt::render)
