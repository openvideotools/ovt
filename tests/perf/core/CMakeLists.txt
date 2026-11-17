# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Core-module benchmarks (chapter 14 §14.9.3 names `bench_core_*` for new rows; WI-0.560 adds only the smoke
# benchmark that validates the harness).

ovt_add_benchmark(core bench_smoke LIBRARIES ovt_core_objects)
ovt_add_benchmark(core bench_container_open_moov_1h LIBRARIES ovt_core_objects)
# WI-1.125: the one-hour 1080p H.264 index (chapter 14 §14.9.3 budget; needs syn_h264_1h_1080p in $OVT_CORPORA_DIR).
ovt_add_benchmark(core bench_index_h264_1h_1080p LIBRARIES ovt_core_objects)
