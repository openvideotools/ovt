# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Cross-module tests. WI-0.550 owns this file (test framework, CTest labels and test presets).

# WI-0.004: deliberate heap overflow in ASan builds, label `canary` (run with `ctest -L canary`).
ovt_add_sanitizer_canary()
