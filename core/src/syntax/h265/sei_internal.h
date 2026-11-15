// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Per-payload H.265 SEI parsers (chapter 7 §7.3.5 row 14, WI-1.114 parts a and b). Module-internal:
// ovt/cxx/syntax/h265/sei.hpp is the public entry point. Every parser reads through the SeiPayloadReader (so no read
// crosses payloadSize) and throws ParseError on a malformed payload; the caller opens the payload node.

#ifndef OVT_SRC_SYNTAX_H265_SEI_INTERNAL_H
#define OVT_SRC_SYNTAX_H265_SEI_INTERNAL_H

#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "syntax/common/sei_framing.h"

namespace ovt::syntax::h265::detail {

inline constexpr std::string_view kSeiSpecRef = "H.265 D.2";
// SPEC-ASSUMPTION(#570): SYN-H265-0051 (payload whose parameter set is unknown, Warning; the payload is kept as
// Bytes) and SYN-H265-0052 (trailing payload bytes, Info) are proposed table rows mirroring SYN-H264-0030..0032.
inline constexpr std::string_view kMissingSetCode = "SYN-H265-0051";
inline constexpr std::string_view kTrailingCode = "SYN-H265-0052";

/// begin_array_element() / end_array_element() that survives a ParseError thrown inside the element.
class SeiArrayElement {
 public:
  SeiArrayElement(SyntaxBuilder& b, std::string_view name, int index) : b_(b) { b_.begin_array_element(name, index); }
  ~SeiArrayElement() { b_.end_array_element(); }
  SeiArrayElement(const SeiArrayElement&) = delete;
  SeiArrayElement& operator=(const SeiArrayElement&) = delete;
  SeiArrayElement(SeiArrayElement&&) = delete;
  SeiArrayElement& operator=(SeiArrayElement&&) = delete;

 private:
  SyntaxBuilder& b_;
};

/// u(n) element `name[index]` with descriptor text "u(v)" (the width comes from a parameter set).
inline uint32_t sei_uv_at(SeiPayloadReader& r, std::string_view name, int index, unsigned n) {
  const SeiArrayElement e(r.builder(), name, index);
  const auto v = static_cast<uint32_t>(r.u(name, n));
  r.builder().set_descriptor_text("u(v)");
  return v;
}

/// u(n) element `name` with descriptor text "u(v)".
inline uint32_t sei_uv(SeiPayloadReader& r, std::string_view name, unsigned n) {
  const auto v = static_cast<uint32_t>(r.u(name, n));
  r.builder().set_descriptor_text("u(v)");
  return v;
}

/// ue(v) element `name[index]`.
inline uint32_t sei_ue_at(SeiPayloadReader& r, std::string_view name, int index) {
  const SeiArrayElement e(r.builder(), name, index);
  return r.ue(name);
}

/// H.265 D.2.1 payload_extension_present(): true when bits other than the payload_bit_equal_to_one /
/// payload_bit_equal_to_zero run are left before the payload end.
[[nodiscard]] bool payload_extension_present(SeiPayloadReader& r);

/// The hrd_parameters() of an SPS's VUI, or null when it has none.
[[nodiscard]] inline const HrdParameters* sps_hrd(const Sps& sps) noexcept {
  if (!sps.vui_parameters_present_flag || !sps.vui || !sps.vui->vui_hrd_parameters_present_flag ||
      !sps.vui->hrd_parameters) {
    return nullptr;
  }
  return &*sps.vui->hrd_parameters;
}

// sei_timing.cpp
BufferingPeriod parse_buffering_period(SeiPayloadReader& r, const Sps& sps);  // type 0; `sps` named by the payload
PicTiming parse_pic_timing(SeiPayloadReader& r, const Sps& sps);              // type 1
RecoveryPoint parse_recovery_point(SeiPayloadReader& r, const Sps* sps);      // type 6; `sps` only for the range
TimeCode parse_time_code(SeiPayloadReader& r);                                // type 136

// sei_parameter_sets.cpp
/// Type 129. `vps_by_id` resolves active_video_parameter_set_id for the layer_sps_idx loop.
ActiveParameterSets parse_active_parameter_sets(SeiPayloadReader& r, std::span<const Vps* const> vps_by_id);
DecodedPictureHash parse_decoded_picture_hash(SeiPayloadReader& r, const Sps& sps);  // type 132

// sei_user_data.cpp
UserDataRegisteredItuTT35 parse_user_data_registered_itu_t_t35(SeiPayloadReader& r);  // type 4
SeiUserDataUnregistered parse_user_data_unregistered_sei(SeiPayloadReader& r);        // type 5

// sei_picture.cpp
FramePackingArrangement parse_frame_packing_arrangement(SeiPayloadReader& r);                        // type 45
DisplayOrientation parse_display_orientation(SeiPayloadReader& r);                                   // type 47
MasteringDisplayColourVolume parse_mastering_display_colour_volume(SeiPayloadReader& r);             // type 137
ContentLightLevelInfo parse_content_light_level_info(SeiPayloadReader& r);                           // type 144
AlternativeTransferCharacteristics parse_alternative_transfer_characteristics(SeiPayloadReader& r);  // type 147
AmbientViewingEnvironment parse_ambient_viewing_environment(SeiPayloadReader& r);                    // type 148

}  // namespace ovt::syntax::h265::detail

#endif  // OVT_SRC_SYNTAX_H265_SEI_INTERNAL_H
