# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Metrics benchmarks (chapter 14 §14.9.3 rows bench_metrics_*). WI-3.590 adds bench_metrics_psnr_1080p,
# bench_metrics_psnr_ssim_1080p and bench_metrics_maps; WI-3.591 to WI-3.593 add their rows to the same binary.
# ovt_add_benchmark() is defined by tests/perf/CMakeLists.txt (WI-0.560b), which adds this directory.

ovt_add_benchmark(metrics bench_metrics LIBRARIES ovt_core_objects)
