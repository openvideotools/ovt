// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 sei_rbsp() and the payload dispatch (chapter 7 §7.3.4 rows 7 and 18, WI-1.110). The payload name table is
// H.264 (08/2021) Annex D, payloadType 0..56 plus the three types above 56 that OVT parses.

#include "ovt/cxx/syntax/h264/sei.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "syntax/common/sei_framing.h"
#include "syntax/h264/h264_internal.h"
#include "syntax/h264/sei_internal.h"

namespace ovt::syntax::h264 {

namespace {

// clang-format off
constexpr std::array<std::string_view, 57> kPayloadNames{
    "buffering_period", "pic_timing", "pan_scan_rect", "filler_payload", "user_data_registered_itu_t_t35",
    "user_data_unregistered", "recovery_point", "dec_ref_pic_marking_repetition", "spare_pic", "scene_info",
    "sub_seq_info", "sub_seq_layer_characteristics", "sub_seq_characteristics", "full_frame_freeze",
    "full_frame_freeze_release", "full_frame_snapshot", "progressive_refinement_segment_start",
    "progressive_refinement_segment_end", "motion_constrained_slice_group_set", "film_grain_characteristics",
    "deblocking_filter_display_preference", "stereo_video_info", "post_filter_hint", "tone_mapping_info",
    "scalability_info", "sub_pic_scalable_layer", "non_required_layer_rep", "priority_layer_info",
    "layers_not_present", "layer_dependency_change", "scalable_nesting", "base_layer_temporal_hrd",
    "quality_layer_integrity_check", "redundant_pic_property", "tl0_dep_rep_index", "tl_switching_point",
    "parallel_decoding_info", "mvc_scalable_nesting", "view_scalability_info", "multiview_scene_info",
    "multiview_acquisition_info", "non_required_view_component", "view_dependency_change",
    "operation_points_not_present", "base_view_temporal_hrd", "frame_packing_arrangement", "multiview_view_position",
    "display_orientation", "mvcd_scalable_nesting", "mvcd_view_scalability_info", "depth_representation_info",
    "three_dimensional_reference_displays_info", "depth_timing", "depth_sampling_info",
    "constrained_depth_parameter_set_identifier", "reserved_sei_message", "green_metadata",};  // 55 is unassigned in H.264 (08/2021)
// clang-format on

constexpr uint32_t kMdcv = 137;
constexpr uint32_t kCll = 144;
constexpr uint32_t kAlternativeTransfer = 147;

// Bytes node for a payload without a parser; the node carries the Annex D name.
void keep_as_bytes(SeiPayloadReader& r, std::string_view name) {
  (void)r.bytes(name, r.payload_size());
}

template <class F>
void parse_into(SeiMessage& m, SyntaxBuilder& b, std::string_view name, F&& fn) {
  auto s = b.scoped(name, detail::kSeiSpecRef);
  m.payload = std::forward<F>(fn)();
  m.parsed = true;
}

}  // namespace

std::string_view sei_payload_name(uint32_t payload_type) noexcept {
  if (payload_type < kPayloadNames.size()) {
    return kPayloadNames[payload_type];
  }
  switch (payload_type) {
    case kMdcv:
      return "mastering_display_colour_volume";
    case kCll:
      return "content_light_level_info";
    case kAlternativeTransfer:
      return "alternative_transfer_characteristics";
    default:
      return "reserved_sei_message";
  }
}

SeiRbsp parse_sei_rbsp(BitReader& br, SyntaxBuilder& b, SeiContext& ctx) {
  SeiRbsp out;
  const BitReader::Mark start = br.mark();
  auto root = b.scoped("sei_rbsp", "H.264 7.3.2.3");
  out.complete = parse_sei_messages(
      br, b,
      [&](const SeiMessageHeader& h, SeiPayloadReader& r) {
        SeiMessage& m = out.messages.emplace_back();
        m.payload_type = h.payload_type;
        m.payload_size = h.payload_size;
        m.name = sei_payload_name(h.payload_type);
        const uint64_t payload_start = r.start_bit();
        m.payload_span = BitSpan{payload_start, static_cast<uint32_t>(uint64_t{h.payload_size} * 8)};
        switch (h.payload_type) {
          case 0: {  // buffering_period: names its SPS in the first element
            BitReader peek(br.bytes(payload_start / 8, h.payload_size));  // not recorded, bounded to the payload
            const uint32_t id = peek.ue();
            detail::require_range(br, "seq_parameter_set_id", id, 0, 31);
            const Sps* sps = id < ctx.sps_by_id.size() ? ctx.sps_by_id[id] : nullptr;
            if (sps == nullptr) {
              b.diag(Severity::Warning, "SYN-H264-0030", "buffering_period SEI before its SPS; parsed as bytes",
                     "H.264 D.1.2");
              keep_as_bytes(r, m.name);
              break;
            }
            parse_into(m, b, m.name, [&] { return detail::parse_buffering_period(r, *sps); });
            ctx.buffering_period_sps_id = id;
            break;
          }
          case 1: {  // pic_timing: the SPS of the previous picture unless a buffering_period of this AU names one
            const Sps* sps = ctx.active_sps;
            if (ctx.buffering_period_sps_id && *ctx.buffering_period_sps_id < ctx.sps_by_id.size() &&
                ctx.sps_by_id[*ctx.buffering_period_sps_id] != nullptr) {
              sps = ctx.sps_by_id[*ctx.buffering_period_sps_id];
            }
            if (sps == nullptr) {
              b.diag(Severity::Warning, "SYN-H264-0031", "pic_timing SEI without an active SPS; parsed as bytes",
                     "H.264 D.2.2");
              keep_as_bytes(r, m.name);
              break;
            }
            parse_into(m, b, m.name, [&] { return detail::parse_pic_timing(r, sps); });
            break;
          }
          case 4:
            parse_into(m, b, m.name, [&] { return detail::parse_user_data_registered_itu_t_t35(r); });
            break;
          case 5:
            parse_into(m, b, m.name, [&] { return detail::parse_user_data_unregistered_sei(r); });
            break;
          case 6:
            parse_into(m, b, m.name, [&] { return detail::parse_recovery_point(r); });
            break;
          case 19:
            parse_into(m, b, m.name, [&] { return detail::parse_film_grain_characteristics(r); });
            break;
          case 23:
            parse_into(m, b, m.name, [&] { return detail::parse_tone_mapping_info(r); });
            break;
          case 45:
            parse_into(m, b, m.name, [&] { return detail::parse_frame_packing_arrangement(r); });
            break;
          case 47:
            parse_into(m, b, m.name, [&] { return detail::parse_display_orientation(r); });
            break;
          case kMdcv:
            parse_into(m, b, m.name, [&] { return detail::parse_mastering_display_colour_volume(r); });
            break;
          case kCll:
            parse_into(m, b, m.name, [&] { return detail::parse_content_light_level_info(r); });
            break;
          case kAlternativeTransfer:
            parse_into(m, b, m.name, [&] { return detail::parse_alternative_transfer_characteristics(r); });
            break;
          default:  // row 18: the tree still shows what is present
            keep_as_bytes(r, m.name);
            break;
        }
      },
      "H.264 7.3.2.3.1", "H.264 7.3.2.11", "SYN-H264-0032");
  out.span = br.span_since(start);
  return out;
}

}  // namespace ovt::syntax::h264
