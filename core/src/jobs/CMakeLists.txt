# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `jobs` module of libovt-core (chapter 0 §0.3, chapter 7 §7.1.6): ovt::jobs::{Cancel,Pool,JobHandle}
# and the ovt_Job runner every blocking C entry point uses (WI-1.512). Sources go into ovt_core_objects.

target_sources(ovt_core_objects PRIVATE pool.cpp job.cpp)
