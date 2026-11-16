# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Core-module benchmarks (chapter 14 §14.9.3 names `bench_core_*` for new rows; WI-0.560 adds only the smoke
# benchmark that validates the harness).

ovt_add_benchmark(core bench_smoke LIBRARIES ovt_core_objects)
