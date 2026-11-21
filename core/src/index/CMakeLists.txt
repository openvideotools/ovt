# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `index` module of libovt-core (chapter 0 §0.3, chapter 7 §7.4). Sources go into ovt_core_objects, which
# ovt_core absorbs (#62). Module-internal headers live next to the sources and are included as "index/<name>.h".

# WI-1.115: H.265 frame boundary, POC and RPS/DPB models (chapter 7 §7.3.9, §7.3.10.2, §7.4.4).
target_sources(ovt_core_objects PRIVATE h265_frame_boundary.cpp h265_poc.cpp h265_dpb.cpp)

# WI-1.119: AV1 frame / temporal-unit grouping (§7.3.9) and the slot model step of §7.4.3 / §7.4.4.
target_sources(ovt_core_objects PRIVATE av1_frame_boundary.cpp av1_slots.cpp)

# WI-4.109: H.264 / H.265 / AV1 level-limit conformance checks (SYN-CONF-0001, SYN-CONF-0002).
target_sources(ovt_core_objects PRIVATE level_limits.cpp)

# WI-1.111: H.264 frame boundary and POC models (chapter 7 §7.3.9, §7.3.10.1).
target_sources(ovt_core_objects PRIVATE h264_frame_boundary.cpp h264_poc.cpp)

# WI-1.122: H.264 DPB simulation (§7.4.4: reference lists, sliding window, MMCO, frame_num gaps).
target_sources(ovt_core_objects PRIVATE h264_dpb.cpp)

# WI-1.121: StreamIndex builder (§7.4.2): core state machine, AV1 dispatch, classification and display order.
target_sources(ovt_core_objects PRIVATE stream_index_builder.cpp av1_units.cpp h264_units.cpp h265_units.cpp
                                        frame_classify.cpp display_order.cpp rap_map.cpp source_hash.cpp)

# WI-1.125: lazy header trees and lazy EPB stripping (§7.4.5, §7.4.6), the on-demand re-parse of one unit.
target_sources(ovt_core_objects PRIVATE lazy_syntax.cpp)
