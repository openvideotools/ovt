// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Per-payload H.264 SEI parsers (chapter 7 §7.3.4 rows 8–18, WI-1.110). Module-internal: ovt/cxx/syntax/h264/sei.hpp
// is the public entry point. Every parser opens its payload node, reads through the SeiPayloadReader (so no
// read crosses payloadSize) and throws ParseError on a malformed payload.

#ifndef OVT_SRC_SYNTAX_H264_SEI_INTERNAL_H
#define OVT_SRC_SYNTAX_H264_SEI_INTERNAL_H

#include <cstdint>
#include <optional>
#include <string>

#include "ovt/cxx/syntax/h264/sei.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "syntax/common/sei_framing.h"

namespace ovt::syntax::h264::detail {

inline constexpr std::string_view kSeiSpecRef = "H.264 D.1";
// SPEC-ASSUMPTION(#528): SYN-H264-0031 (pic_timing without an SPS, Warning) and SYN-H264-0032 (trailing payload bytes,
// Info) are proposed table rows; 0030 is the buffering_period code of §7.3.4 row 8.

/// begin_array_element() / end_array_element() that survives a ParseError thrown inside the element.
class ArrayElementScope {
 public:
  ArrayElementScope(SyntaxBuilder& b, std::string_view name, int index) : b_(b) { b_.begin_array_element(name, index); }
  ~ArrayElementScope() { b_.end_array_element(); }
  ArrayElementScope(const ArrayElementScope&) = delete;
  ArrayElementScope& operator=(const ArrayElementScope&) = delete;

 private:
  SyntaxBuilder& b_;
};

// sei_timing.cpp
/// Row 8. `sps` is the SPS named by the message's seq_parameter_set_id (already looked up by the caller).
BufferingPeriod parse_buffering_period(SeiPayloadReader& r, const Sps& sps);
/// Row 9. `sps` may be null: only the framing is kept then.
PicTiming parse_pic_timing(SeiPayloadReader& r, const Sps* sps);

// sei_picture.cpp
RecoveryPoint parse_recovery_point(SeiPayloadReader& r);
FramePackingArrangement parse_frame_packing_arrangement(SeiPayloadReader& r);
DisplayOrientation parse_display_orientation(SeiPayloadReader& r);
MasteringDisplayColourVolume parse_mastering_display_colour_volume(SeiPayloadReader& r);
ContentLightLevelInfo parse_content_light_level_info(SeiPayloadReader& r);
AlternativeTransferCharacteristics parse_alternative_transfer_characteristics(SeiPayloadReader& r);
FilmGrainCharacteristics parse_film_grain_characteristics(SeiPayloadReader& r);
ToneMappingInfo parse_tone_mapping_info(SeiPayloadReader& r);

// sei_user_data.cpp
UserDataRegisteredItuTT35 parse_user_data_registered_itu_t_t35(SeiPayloadReader& r);
SeiUserDataUnregistered parse_user_data_unregistered_sei(SeiPayloadReader& r);

}  // namespace ovt::syntax::h264::detail

#endif  // OVT_SRC_SYNTAX_H264_SEI_INTERNAL_H
