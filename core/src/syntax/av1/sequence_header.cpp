// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 sequence_header_obu() (chapter 7 §7.3.6 row 2; WI-1.116). The function mirrors AV1 §5.5.1 line by line;
// the values the table infers are written into the struct and, where the row asks for it, emitted as derived
// nodes so the tree shows what a decoder would assume.

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecSequenceHeader = "AV1 5.5.1";
constexpr uint8_t kLevelMaxParameters = 31;

std::string_view profile_name_of(uint8_t seq_profile) noexcept {
  switch (seq_profile) {
    case 0:
      return "Main";
    case 1:
      return "High";
    case 2:
      return "Professional";
    default:
      return "Reserved";
  }
}

// The operating point loop body of AV1 §5.5.1, for operating point i.
void parse_operating_point(BitReader& br, SyntaxBuilder& b, SequenceHeader& seq, int i) {
  OperatingPoint op;
  b.begin_array_element("operating_point", i, kSpecSequenceHeader);
  op.operating_point_idc = static_cast<uint16_t>(b.read_u("operating_point_idc", 12));
  op.seq_level_idx = static_cast<uint8_t>(b.read_u("seq_level_idx", 5));
  op.level_name = level_name(op.seq_level_idx);
  b.set_enum(op.level_name);
  if (op.seq_level_idx > 7) {  // the tier bit is read for every level above 3.3, 31 included (pitfalls)
    op.seq_tier = b.read_flag("seq_tier");
  } else {
    op.seq_tier = false;
  }
  op.tier_name = tier_name(op.seq_tier);
  b.derived("level", fmt::format("level = {} {}", op.level_name, op.tier_name));
  if (seq.decoder_model_info_present_flag) {
    op.decoder_model_present_for_this_op = b.read_flag("decoder_model_present_for_this_op");
    if (op.decoder_model_present_for_this_op) {
      op.operating_parameters_info = parse_operating_parameters_info(br, b, i, seq.decoder_model_info);
    }
  } else {
    op.decoder_model_present_for_this_op = false;
  }
  if (seq.initial_display_delay_present_flag) {
    op.initial_display_delay_present_for_this_op = b.read_flag("initial_display_delay_present_for_this_op");
    if (op.initial_display_delay_present_for_this_op) {
      op.initial_display_delay_minus_1 = static_cast<uint8_t>(b.read_u("initial_display_delay_minus_1", 4));
    }
  }
  b.end_array_element();
  seq.operating_points.push_back(std::move(op));
}

}  // namespace

std::string level_name(uint8_t seq_level_idx) {
  if (seq_level_idx == kLevelMaxParameters) {
    return "Maximum parameters";
  }
  // AV1 Annex A.3: seq_level_idx = (X - 2) * 4 + Y for level X.Y; levels 2.0 … 7.3 are defined, the others
  // are reserved (A.3 table).
  const unsigned major = 2U + (seq_level_idx >> 2U);
  const unsigned minor = seq_level_idx & 3U;
  if (major > 7) {
    return "Reserved";
  }
  return fmt::format("{}.{}", major, minor);
}

std::string_view tier_name(bool seq_tier) noexcept {
  return seq_tier ? "High" : "Main";
}

uint16_t SequenceHeader::operating_point_idc(size_t index) const noexcept {
  return index < operating_points.size() ? operating_points[index].operating_point_idc : 0;
}

namespace {

// still_picture handling up to and including the operating points (AV1 §5.5.1): the reduced_still_picture_header
// path has one implicit operating point, the full path reads timing_info, decoder_model_info and the list.
void parse_operating_points(BitReader& br, SyntaxBuilder& b, SequenceHeader& seq) {
  if (seq.reduced_still_picture_header) {
    seq.timing_info_present_flag = false;
    seq.decoder_model_info_present_flag = false;
    seq.initial_display_delay_present_flag = false;
    seq.operating_points_cnt_minus_1 = 0;
    OperatingPoint op;
    op.operating_point_idc = 0;
    b.begin_array_element("operating_point", 0, kSpecSequenceHeader);
    op.seq_level_idx = static_cast<uint8_t>(b.read_u("seq_level_idx", 5));
    op.level_name = level_name(op.seq_level_idx);
    b.set_enum(op.level_name);
    op.seq_tier = false;
    op.tier_name = tier_name(false);
    b.end_array_element();
    seq.operating_points.push_back(std::move(op));
    b.derived("reduced_still_picture_header",
              "timing_info_present_flag = 0, decoder_model_info_present_flag = 0, "
              "initial_display_delay_present_flag = 0, operating_points_cnt_minus_1 = 0, "
              "operating_point_idc[0] = 0, seq_tier[0] = 0");
  } else {
    seq.timing_info_present_flag = b.read_flag("timing_info_present_flag");
    if (seq.timing_info_present_flag) {
      seq.timing_info = parse_timing_info(br, b);
      seq.decoder_model_info_present_flag = b.read_flag("decoder_model_info_present_flag");
      if (seq.decoder_model_info_present_flag) {
        seq.decoder_model_info = parse_decoder_model_info(br, b);
      }
    } else {
      seq.decoder_model_info_present_flag = false;
    }
    seq.initial_display_delay_present_flag = b.read_flag("initial_display_delay_present_flag");
    seq.operating_points_cnt_minus_1 = static_cast<uint8_t>(b.read_u("operating_points_cnt_minus_1", 5));
    seq.operating_points.reserve(seq.operating_points_cnt_minus_1 + 1U);
    for (int i = 0; i <= seq.operating_points_cnt_minus_1; ++i) {
      parse_operating_point(br, b, seq, i);
    }
  }
}

// enable_* tools, screen content and integer mv selection and OrderHintBits (AV1 §5.5.1); all fixed for the
// reduced_still_picture_header path.
void parse_inter_tools(SyntaxBuilder& b, SequenceHeader& seq) {
  if (seq.reduced_still_picture_header) {
    seq.enable_interintra_compound = false;
    seq.enable_masked_compound = false;
    seq.enable_warped_motion = false;
    seq.enable_dual_filter = false;
    seq.enable_order_hint = false;
    seq.enable_jnt_comp = false;
    seq.enable_ref_frame_mvs = false;
    seq.seq_force_screen_content_tools = kSelectScreenContentTools;
    seq.seq_force_integer_mv = kSelectIntegerMv;
    seq.OrderHintBits = 0;
    b.derived("inter_tools",
              "enable_interintra_compound = 0, enable_masked_compound = 0, enable_warped_motion = 0, "
              "enable_dual_filter = 0, enable_order_hint = 0, enable_jnt_comp = 0, enable_ref_frame_mvs = 0, "
              "seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS, "
              "seq_force_integer_mv = SELECT_INTEGER_MV, OrderHintBits = 0");
  } else {
    seq.enable_interintra_compound = b.read_flag("enable_interintra_compound");
    seq.enable_masked_compound = b.read_flag("enable_masked_compound");
    seq.enable_warped_motion = b.read_flag("enable_warped_motion");
    seq.enable_dual_filter = b.read_flag("enable_dual_filter");
    seq.enable_order_hint = b.read_flag("enable_order_hint");
    if (seq.enable_order_hint) {
      seq.enable_jnt_comp = b.read_flag("enable_jnt_comp");
      seq.enable_ref_frame_mvs = b.read_flag("enable_ref_frame_mvs");
    } else {
      seq.enable_jnt_comp = false;
      seq.enable_ref_frame_mvs = false;
    }
    seq.seq_choose_screen_content_tools = b.read_flag("seq_choose_screen_content_tools");
    if (seq.seq_choose_screen_content_tools) {
      seq.seq_force_screen_content_tools = kSelectScreenContentTools;
    } else {
      seq.seq_force_screen_content_tools = static_cast<uint8_t>(b.read_u("seq_force_screen_content_tools", 1));
    }
    if (seq.seq_force_screen_content_tools > 0) {
      seq.seq_choose_integer_mv = b.read_flag("seq_choose_integer_mv");
      if (seq.seq_choose_integer_mv) {
        seq.seq_force_integer_mv = kSelectIntegerMv;
      } else {
        seq.seq_force_integer_mv = static_cast<uint8_t>(b.read_u("seq_force_integer_mv", 1));
      }
    } else {
      seq.seq_force_integer_mv = kSelectIntegerMv;
    }
    b.derived("seq_force_screen_content_tools", seq.seq_force_screen_content_tools);
    b.derived("seq_force_integer_mv", seq.seq_force_integer_mv);
    if (seq.enable_order_hint) {
      seq.order_hint_bits_minus_1 = static_cast<uint8_t>(b.read_u("order_hint_bits_minus_1", 3));
      seq.OrderHintBits = seq.order_hint_bits_minus_1 + 1U;
    } else {
      seq.OrderHintBits = 0;
    }
  }
}

}  // namespace

SequenceHeader parse_sequence_header(BitReader& br, SyntaxBuilder& b) {
  SequenceHeader seq;
  const auto start = br.mark();
  auto s = b.scoped("sequence_header_obu", kSpecSequenceHeader);
  seq.seq_profile = static_cast<uint8_t>(b.read_u("seq_profile", 3));
  seq.profile_name = profile_name_of(seq.seq_profile);
  b.set_enum(seq.profile_name);
  if (seq.seq_profile > 2) {  // §7.3.6 row 2: > 2 → ParseError, before anything else is read
    throw ParseError{"SYN-GEN-0003", fmt::format("seq_profile: forbidden/reserved value {}", seq.seq_profile),
                     br.bit_pos()};
  }
  seq.still_picture = b.read_flag("still_picture");
  seq.reduced_still_picture_header = b.read_flag("reduced_still_picture_header");
  parse_operating_points(br, b, seq);
  b.derived("operating_point_count", seq.operating_points.size());
  seq.frame_width_bits_minus_1 = static_cast<uint8_t>(b.read_u("frame_width_bits_minus_1", 4));
  seq.frame_height_bits_minus_1 = static_cast<uint8_t>(b.read_u("frame_height_bits_minus_1", 4));
  seq.max_frame_width_minus_1 =
      static_cast<uint32_t>(b.read_u("max_frame_width_minus_1", seq.frame_width_bits_minus_1 + 1U));
  b.set_descriptor_text("f(n)");
  seq.max_frame_height_minus_1 =
      static_cast<uint32_t>(b.read_u("max_frame_height_minus_1", seq.frame_height_bits_minus_1 + 1U));
  b.set_descriptor_text("f(n)");
  seq.max_frame_width = seq.max_frame_width_minus_1 + 1;
  seq.max_frame_height = seq.max_frame_height_minus_1 + 1;
  b.derived("max_frame_size", fmt::format("max_frame_size = {}x{}", seq.max_frame_width, seq.max_frame_height));
  if (seq.reduced_still_picture_header) {
    seq.frame_id_numbers_present_flag = false;
  } else {
    seq.frame_id_numbers_present_flag = b.read_flag("frame_id_numbers_present_flag");
  }
  if (seq.frame_id_numbers_present_flag) {
    seq.delta_frame_id_length_minus_2 = static_cast<uint8_t>(b.read_u("delta_frame_id_length_minus_2", 4));
    seq.additional_frame_id_length_minus_1 = static_cast<uint8_t>(b.read_u("additional_frame_id_length_minus_1", 3));
  }
  seq.use_128x128_superblock = b.read_flag("use_128x128_superblock");
  seq.enable_filter_intra = b.read_flag("enable_filter_intra");
  seq.enable_intra_edge_filter = b.read_flag("enable_intra_edge_filter");
  parse_inter_tools(b, seq);
  b.derived("OrderHintBits", seq.OrderHintBits);
  seq.enable_superres = b.read_flag("enable_superres");
  seq.enable_cdef = b.read_flag("enable_cdef");
  seq.enable_restoration = b.read_flag("enable_restoration");
  seq.color_config = parse_color_config(br, b, seq.seq_profile);
  seq.film_grain_params_present = b.read_flag("film_grain_params_present");
  parse_trailing_bits(br, b);
  seq.span = br.span_since(start);
  return seq;
}

}  // namespace ovt::syntax::av1
