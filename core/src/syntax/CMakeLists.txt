# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `syntax` module of libovt-core (chapter 0 §0.3, chapter 7 §7.3). Sources go into ovt_core_objects, which
# ovt_core absorbs (#62).

# Shared user-data registries (chapter 7 §7.3.11, WI-1.120).
target_sources(ovt_core_objects PRIVATE common/user_data_registry.cpp common/t35_hdr10plus.cpp common/t35_atsc_cc.cpp)

# Unit scanners: Annex B, length-prefixed and AV1 OBU streams (chapter 7 §7.3.3, WI-1.107).
target_sources(ovt_core_objects PRIVATE common/unit_scan.cpp)

# WI-1.106: SyntaxBuilder, the arena tree SyntaxNodeBuffer with its ovt.fb.SyntaxNode conversion, and the JSON and
# trace writers.
target_sources(ovt_core_objects PRIVATE common/syntax_builder.cpp common/syntax_node_buffer.cpp common/syntax_dump.cpp)
# common/syntax_node_buffer.h includes the generated ovt_model_generated.h, and the module tests include that
# header, so the generated include directory and flatbuffers are PUBLIC on the objects, inside BUILD_INTERFACE
# (core-objects rule 1, #62).
target_link_libraries(ovt_core_objects PUBLIC "$<BUILD_INTERFACE:ovt_schemas>")

# WI-1.112: H.265 header parsers (chapter 7 §7.3.5 rows 1–9); typed structs live in core/include/ovt/cxx/syntax/h265/
# (§7.3.7). Part a: nal_unit_header, profile_tier_level, hrd_parameters.
target_sources(ovt_core_objects PRIVATE h265/nal_header.cpp h265/profile_tier_level.cpp h265/hrd_parameters.cpp)
