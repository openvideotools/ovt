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

# WI-1.116: AV1 OBU header, sequence header (timing, decoder model, color_config), temporal delimiter, padding,
# metadata and tile list parsers (chapter 7 §7.3.6 rows 1–9 and 30).
target_sources(ovt_core_objects PRIVATE av1/obu_header.cpp av1/sequence_header.cpp av1/color_config.cpp av1/timing.cpp
                                        av1/metadata.cpp av1/tile_list.cpp)
# WI-1.119: the AV1 reference slot model (DecoderState, §7.20 update, §7.21 loading, §7.8 set_frame_refs).
target_sources(ovt_core_objects PRIVATE av1/decoder_state.cpp)
# WI-1.117: AV1 uncompressed_header() part 1 (chapter 7 §7.3.6 rows 11–19): frame_size / superres / render_size /
# frame_size_with_refs (rows 12–14), tile_info (row 15), quantization / segmentation / delta params (rows 16–19)
# and uncompressed_header() itself (row 11).
target_sources(ovt_core_objects PRIVATE av1/frame_size.cpp av1/tile_info.cpp av1/quantization.cpp av1/segmentation.cpp
                                        av1/delta_params.cpp av1/frame_header.cpp)
# WI-1.118: uncompressed_header() rows 20–29 (loop filter, CDEF, LR, tx mode, reference / skip mode, global motion,
# film grain, frame OBU, tile group).
target_sources(ovt_core_objects PRIVATE av1/loop_filter.cpp av1/cdef.cpp av1/lr.cpp av1/tx_mode.cpp av1/skip_mode.cpp
                                        av1/global_motion.cpp av1/film_grain.cpp)
target_sources(ovt_core_objects PRIVATE av1/frame_obu.cpp av1/tile_group.cpp)
# WI-1.112: H.265 NAL header, profile_tier_level, hrd_parameters, vui_parameters, scaling_list_data,
# st_ref_pic_set (shared with WI-1.113), VPS, SPS and PPS parsers with the ActivePps tile tables (chapter 7 §7.3.5
# rows 1–9). Typed structs live in core/include/ovt/cxx/syntax/h265/ (§7.3.7).
target_sources(
  ovt_core_objects
  PRIVATE h265/nal_header.cpp
          h265/profile_tier_level.cpp
          h265/hrd_parameters.cpp
          h265/vui.cpp
          h265/scaling_list.cpp
          h265/st_ref_pic_set.cpp
          h265/vps.cpp
          h265/sps.cpp
          h265/pps.cpp)
# WI-1.113: H.265 slice_segment_header(), ref_pic_lists_modification(), pred_weight_table() and the entry-point
# substream mapping (chapter 7 §7.3.5 rows 10–12).
target_sources(ovt_core_objects PRIVATE h265/slice_segment_header.cpp h265/ref_pic_lists_modification.cpp
                                        h265/pred_weight_table.cpp)
# WI-1.115: H.265 parameter-set activation tracking (chapter 7 §7.3.8).
target_sources(ovt_core_objects PRIVATE h265/active_sets.cpp)
# WI-1.108: H.264 NAL header, SPS (VUI, HRD, scaling lists) and PPS parsers; H.273 code-point names shared with
# the H.265 and AV1 parsers.
target_sources(ovt_core_objects PRIVATE common/h273_names.cpp h264/nal_header.cpp h264/scaling_list.cpp h264/vui.cpp
                                        h264/sps.cpp h264/pps.cpp)
# WI-1.109a: H.264 slice header with ref list modification, pred weight table and dec ref pic marking (chapter 7
# §7.3.4 rows 24–27).
target_sources(ovt_core_objects PRIVATE h264/slice_header.cpp h264/ref_pic_list_modification.cpp
                                        h264/pred_weight_table.cpp h264/dec_ref_pic_marking.cpp)
# WI-1.109b: H.264 AUD, end of sequence / stream, filler data and slice data partitions (chapter 7 §7.3.4 rows 19, 20
# and 28).
target_sources(ovt_core_objects PRIVATE h264/aud.cpp h264/data_partition.cpp)
# WI-1.109c: H.264 SPS extension, subset SPS (MVC extension) and prefix NAL (chapter 7 §7.3.4 rows 21–23).
target_sources(ovt_core_objects PRIVATE h264/sps_ext.cpp h264/subset_sps.cpp h264/prefix_nal.cpp)
# WI-1.110: SEI framing shared by H.264 and H.265, and the H.264 SEI payload parsers (chapter 7 §7.3.4 rows 7–18).
target_sources(ovt_core_objects PRIVATE common/sei_framing.cpp h264/sei.cpp h264/sei_timing.cpp h264/sei_picture.cpp
                                        h264/sei_user_data.cpp)
