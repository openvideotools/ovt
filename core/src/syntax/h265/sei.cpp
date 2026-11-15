// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 sei_rbsp() and the payload dispatch (chapter 7 §7.3.5 rows 13 and 14, WI-1.114). The payload names and the
// prefix / suffix placement follow sei_payload() of H.265 (08/2021) D.2.1.

#include "ovt/cxx/syntax/h265/sei.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "syntax/common/sei_framing.h"
#include "syntax/h265/h265_internal.h"
#include "syntax/h265/sei_internal.h"

namespace ovt::syntax::h265 {

namespace {

constexpr std::string_view kReserved = "reserved_sei_message";

// H.265 (08/2021) D.2.1 payload names; every other payloadType is reserved_sei_message.
struct PayloadName {
  uint32_t type;
  std::string_view name;
};
// clang-format off
constexpr std::array<PayloadName, 67> kPayloadNames{{
    {0, "buffering_period"}, {1, "pic_timing"}, {2, "pan_scan_rect"}, {3, "filler_payload"},
    {4, "user_data_registered_itu_t_t35"}, {5, "user_data_unregistered"}, {6, "recovery_point"}, {9, "scene_info"},
    {15, "picture_snapshot"}, {16, "progressive_refinement_segment_start"}, {17, "progressive_refinement_segment_end"},
    {19, "film_grain_characteristics"}, {22, "post_filter_hint"}, {23, "tone_mapping_info"},
    {45, "frame_packing_arrangement"}, {47, "display_orientation"}, {56, "green_metadata"},
    {128, "structure_of_pictures_info"}, {129, "active_parameter_sets"}, {130, "decoding_unit_info"},
    {131, "temporal_sub_layer_zero_idx"}, {132, "decoded_picture_hash"}, {133, "scalable_nesting"},
    {134, "region_refresh_info"}, {135, "no_display"}, {136, "time_code"}, {137, "mastering_display_colour_volume"},
    {138, "segmented_rect_frame_packing_arrangement"}, {139, "temporal_motion_constrained_tile_sets"},
    {140, "chroma_resampling_filter_hint"}, {141, "knee_function_info"}, {142, "colour_remapping_info"},
    {143, "deinterlaced_field_identification"}, {144, "content_light_level_info"}, {145, "dependent_rap_indication"},
    {146, "coded_region_completion"}, {147, "alternative_transfer_characteristics"},
    {148, "ambient_viewing_environment"}, {149, "content_colour_volume"}, {150, "equirectangular_projection"},
    {151, "cubemap_projection"}, {152, "fisheye_video_info"}, {154, "sphere_rotation"}, {155, "regionwise_packing"},
    {156, "omni_viewport"}, {157, "regional_nesting"}, {158, "mcts_extraction_info_sets"},
    {159, "mcts_extraction_info_nesting"}, {160, "layers_not_present"}, {161, "inter_layer_constrained_tile_sets"},
    {162, "bsp_nesting"}, {163, "bsp_initial_arrival_time"}, {164, "sub_bitstream_property"},
    {165, "alpha_channel_info"}, {166, "overlay_info"}, {167, "temporal_mv_prediction_constraints"},
    {168, "frame_field_info"}, {176, "three_dimensional_reference_displays_info"}, {177, "depth_representation_info"},
    {178, "multiview_scene_info"}, {179, "multiview_acquisition_info"}, {180, "multiview_view_position"},
    {181, "alternative_depth_info"}, {200, "sei_manifest"}, {201, "sei_prefix_indication"}, {202, "annotated_regions"},
    {205, "shutter_interval_info"},
}};
// clang-format on

// Bytes node for a payload without a parser; the node carries the Annex D name.
void keep_as_bytes(SeiPayloadReader& r, std::string_view name) {
  (void)r.bytes(name, r.payload_size());
}

template <class F>
void parse_into(SeiMessage& m, SyntaxBuilder& b, F&& fn) {
  auto s = b.scoped(m.name, detail::kSeiSpecRef);
  m.payload = std::forward<F>(fn)();
  m.parsed = true;
}

void missing_set(SeiMessage& m, SeiPayloadReader& r, std::string_view what) {
  r.builder().diag(Severity::Warning, detail::kMissingSetCode,
                   fmt::format("{} SEI without its {}; parsed as bytes", m.name, what), detail::kSeiSpecRef);
  keep_as_bytes(r, m.name);
}

const Sps* lookup(std::span<const Sps* const> table, uint32_t id) noexcept {
  return id < table.size() ? table[id] : nullptr;
}

// The SPS of the current picture as seen from an SEI message (sei.hpp, SeiContext).
const Sps* picture_sps(const SeiContext& ctx, bool suffix) noexcept {
  if (!suffix && ctx.buffering_period_sps_id) {
    if (const Sps* s = lookup(ctx.sps_by_id, *ctx.buffering_period_sps_id)) {
      return s;
    }
  }
  return ctx.active_sps;
}

}  // namespace

std::string_view sei_payload_name(uint32_t payload_type) noexcept {
  for (const PayloadName& p : kPayloadNames) {
    if (p.type == payload_type) {
      return p.name;
    }
  }
  return kReserved;
}

bool sei_payload_allowed(uint32_t payload_type, bool suffix) noexcept {
  // D.2.1: the suffix branch of sei_payload() lists these types; every other named type is prefix-only.
  const bool suffix_type = payload_type == 3 || payload_type == 4 || payload_type == 5 || payload_type == 17 ||
                           payload_type == 22 || payload_type == 132 || payload_type == 146;
  if (suffix) {
    return suffix_type;
  }
  return payload_type != 132 && payload_type != 146 && sei_payload_name(payload_type) != kReserved;
}

SeiRbsp parse_sei_rbsp(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, SeiContext& ctx) {
  SeiRbsp out;
  out.suffix = nal.nal_unit_type == kSuffixSeiNut;
  const BitReader::Mark start = br.mark();
  auto root = b.scoped("sei_rbsp", "H.265 7.3.2.4");
  out.complete = parse_sei_messages(
      br, b,
      [&](const SeiMessageHeader& h, SeiPayloadReader& r) {
        SeiMessage& m = out.messages.emplace_back();
        m.payload_type = h.payload_type;
        m.payload_size = h.payload_size;
        m.name = sei_payload_name(h.payload_type);
        m.payload_span = BitSpan{r.start_bit(), static_cast<uint32_t>(uint64_t{h.payload_size} * 8)};
        if (m.name != kReserved && !sei_payload_allowed(h.payload_type, out.suffix)) {
          m.allowed = false;
          b.diag(Severity::Warning, "SYN-H265-0050",
                 fmt::format("payload type {} not allowed in {} SEI", h.payload_type, out.suffix ? "suffix" : "prefix"),
                 "H.265 D.2.1");
        }
        switch (h.payload_type) {
          case 0: {  // buffering_period: names its SPS in the first element
            BitReader peek(br.bytes(r.start_bit() / 8, h.payload_size));  // not recorded, bounded to the payload
            const uint32_t id = peek.ue();
            detail::require_range(br, "bp_seq_parameter_set_id", id, 0, 15);
            const Sps* sps = lookup(ctx.sps_by_id, id);
            if (sps == nullptr) {
              missing_set(m, r, "SPS");
              break;
            }
            parse_into(m, b, [&] { return detail::parse_buffering_period(r, *sps); });
            ctx.buffering_period_sps_id = id;
            break;
          }
          case 1:
          case 132: {
            const Sps* sps = picture_sps(ctx, out.suffix);
            if (sps == nullptr) {
              missing_set(m, r, "active SPS");
              break;
            }
            if (h.payload_type == 1) {
              parse_into(m, b, [&] { return detail::parse_pic_timing(r, *sps); });
            } else {
              parse_into(m, b, [&] { return detail::parse_decoded_picture_hash(r, *sps); });
            }
            break;
          }
          case 6:
            parse_into(m, b, [&] { return detail::parse_recovery_point(r, picture_sps(ctx, out.suffix)); });
            break;
          case 129:
            parse_into(m, b, [&] { return detail::parse_active_parameter_sets(r, ctx.vps_by_id); });
            break;
          default:  // row 14: the tree still shows what is present
            keep_as_bytes(r, m.name);
            break;
        }
      },
      "H.265 7.3.5", "H.265 7.3.2.11", detail::kTrailingCode);
  out.span = br.span_since(start);
  return out;
}

namespace detail {

bool payload_extension_present(SeiPayloadReader& r) {
  // The payload ends with payload_bit_equal_to_one and zero bits only when it is not byte aligned; anything before
  // the last set bit of the payload is extension data (D.3.1 semantics of payload_extension_present()).
  const uint64_t left = r.bits_left();
  if (left == 0) {
    return false;
  }
  const BitReader& br = r.reader();
  const uint64_t pos = br.bit_pos();
  const uint64_t end = r.end_bit();
  const std::span<const uint8_t> bytes = br.bytes(pos / 8, (end + 7) / 8 - pos / 8);
  uint64_t last_one = end;  // absolute bit position of the last 1 bit in [pos, end)
  for (uint64_t bit = end; bit-- > pos;) {
    const uint8_t byte = bytes[bit / 8 - pos / 8];
    if (((byte >> (7 - bit % 8)) & 1U) != 0) {
      last_one = bit;
      break;
    }
  }
  if (last_one == end) {
    return false;  // only zero bits left
  }
  return last_one > pos;
}

}  // namespace detail

}  // namespace ovt::syntax::h265
