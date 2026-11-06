# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `api` module of libovt-core (chapter 0 §0.3): the noexcept C entry points of chapter 12 §12.3
# (§12.3.1 rule 8). WI-1.512 adds ovt/log.h (and ovt/jobs.h in its second part); WI-1.101 adds ovt/io.h. Sources go into ovt_core_objects.

target_sources(ovt_core_objects PRIVATE file_api.cpp log_api.cpp)
