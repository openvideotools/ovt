# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `api` module of libovt-core (chapter 0 §0.3): the noexcept C entry points of chapter 12 §12.3
# (§12.3.1 rule 8). WI-1.512 adds ovt/jobs.h and ovt/log.h; WI-1.101 adds ovt/io.h. Sources go into
# ovt_core_objects. WI-3.300 adds buffer.cpp, the allocator of caller-owned ovt_Buffers (rule 3a).

target_sources(ovt_core_objects PRIVATE buffer.cpp cache_api.cpp file_api.cpp jobs_api.cpp log_api.cpp)
