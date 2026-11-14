# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `index` module of libovt-core (chapter 0 §0.3, chapter 7 §7.4). Sources go into ovt_core_objects, which
# ovt_core absorbs (#62). Module-internal headers live next to the sources and are included as "index/<name>.h".

# WI-1.119: AV1 frame / temporal-unit grouping (§7.3.9) and the slot model step of §7.4.3 / §7.4.4.
target_sources(ovt_core_objects PRIVATE av1_frame_boundary.cpp av1_slots.cpp)
