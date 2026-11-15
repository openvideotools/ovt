// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 SEI messages (Annex D): sei_rbsp() over the shared framing of WI-1.110 and the payload parsers of chapter 7
// §7.3.5 rows 13 and 14 (WI-1.114a: buffering_period, pic_timing, recovery_point, active_parameter_sets and
// decoded_picture_hash; WI-1.114b adds time_code and the user-data and colour payloads). Every other payload type is
// kept as Bytes under its Annex D name. C++ helper header (D22): semi-public, no ABI promise, never included from a
// C header.
//
// Every message becomes a node `sei_message[i]` whose child is the payload node named after the payload. A payload
// type that D.2.1 does not allow in this SEI NAL unit type is still parsed, with Diagnostic Warning SYN-H265-0050.

#ifndef OVT_CXX_SYNTAX_H265_SEI_HPP
#define OVT_CXX_SYNTAX_H265_SEI_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// One CPB of buffering_period() (D.2.2), NAL or VCL HRD; the alt fields only when BufferingPeriod::alt_present.
struct InitialCpbRemoval {
  uint32_t initial_cpb_removal_delay = 0;
  uint32_t initial_cpb_removal_offset = 0;
  uint32_t initial_alt_cpb_removal_delay = 0;
  uint32_t initial_alt_cpb_removal_offset = 0;
};

/// buffering_period() (D.2.2, type 0). Field lengths come from the hrd_parameters() of the SPS it names.
struct BufferingPeriod {
  uint32_t bp_seq_parameter_set_id = 0;
  bool irap_cpb_params_present_flag = false;  // inferred 0 when sub_pic_hrd_params_present_flag
  uint32_t cpb_delay_offset = 0;
  uint32_t dpb_delay_offset = 0;
  bool concatenation_flag = false;
  uint32_t au_cpb_removal_delay_delta_minus1 = 0;
  bool alt_present = false;                     // the *_alt_* fields of the CPB entries were sent
  std::vector<InitialCpbRemoval> nal;           // CpbCnt entries when NalHrdBpPresentFlag, else empty
  std::vector<InitialCpbRemoval> vcl;           // CpbCnt entries when VclHrdBpPresentFlag, else empty
  std::optional<bool> use_alt_cpb_params_flag;  // present only with payload_extension_present()
};

/// pic_timing() (D.2.3, type 1).
struct PicTiming {
  bool frame_field_info_present = false;  // the VUI frame_field_info_present_flag
  uint32_t pic_struct = 0;
  std::string_view pic_struct_name;  // Table D-2 (static storage)
  uint32_t source_scan_type = 0;
  bool duplicate_flag = false;
  bool cpb_dpb_delays_present = false;  // CpbDpbDelaysPresentFlag
  uint32_t au_cpb_removal_delay_minus1 = 0;
  uint32_t pic_dpb_output_delay = 0;
  uint32_t pic_dpb_output_du_delay = 0;  // sub_pic_hrd_params_present_flag
  bool du_info_present = false;          // sub_pic_hrd_params_present_flag && sub_pic_cpb_params_in_pic_timing_sei_flag
  uint32_t num_decoding_units_minus1 = 0;
  bool du_common_cpb_removal_delay_flag = false;
  uint32_t du_common_cpb_removal_delay_increment_minus1 = 0;
  std::vector<uint32_t> num_nalus_in_du_minus1;                 // num_decoding_units_minus1 + 1 entries
  std::vector<uint32_t> du_cpb_removal_delay_increment_minus1;  // num_decoding_units_minus1 entries, or none
};

/// recovery_point() (D.2.8, type 6).
struct RecoveryPoint {
  int32_t recovery_poc_cnt = 0;
  bool exact_match_flag = false;
  bool broken_link_flag = false;
};

/// active_parameter_sets() (D.2.21, type 129).
struct ActiveParameterSets {
  uint32_t active_video_parameter_set_id = 0;
  bool self_contained_cvs_flag = false;
  bool no_parameter_set_update_flag = false;
  uint32_t num_sps_ids_minus1 = 0;
  std::vector<uint32_t> active_seq_parameter_set_id;  // num_sps_ids_minus1 + 1 entries
  /// layer_sps_idx[i] for i = vps_base_layer_internal_flag..MaxLayersMinus1 (entry k is layer_sps_idx[first_layer +
  /// k]); empty for a single-layer VPS. Without the VPS the entries the payload holds are read, from index 0, with
  /// Diagnostic Warning SYN-H265-0051.
  std::vector<uint32_t> layer_sps_idx;
  uint32_t first_layer = 0;  // vps_base_layer_internal_flag of the named VPS
  bool vps_known = false;
};

/// decoded_picture_hash() (D.3.19, type 132), stored for the verify job (§7.6.4).
struct DecodedPictureHash {
  uint32_t hash_type = 0;  // 0 MD5, 1 CRC, 2 checksum; other values are reserved (the components are not read)
  std::string_view hash_type_name;
  uint32_t num_components = 0;                           // chroma_format_idc == 0 ? 1 : 3
  std::array<std::array<uint8_t, 16>, 3> picture_md5{};  // hash_type 0
  std::array<uint32_t, 3> picture_crc{};                 // hash_type 1 (u(16))
  std::array<uint32_t, 3> picture_checksum{};            // hash_type 2
};

/// The parsed form of one message; std::monostate when the payload is kept as bytes.
using SeiPayload =
    std::variant<std::monostate, BufferingPeriod, PicTiming, RecoveryPoint, ActiveParameterSets, DecodedPictureHash>;

struct SeiMessage {
  uint32_t payload_type = 0;
  uint32_t payload_size = 0;
  std::string_view name;  // Annex D name or "reserved_sei_message" (static storage)
  BitSpan payload_span{0, 0};
  bool allowed = true;  // false: D.2.1 does not allow the type in this SEI NAL unit type (SYN-H265-0050)
  bool parsed = false;  // false: kept as Bytes (no parser, parameter set missing) or the payload failed to parse
  SeiPayload payload;
};

/// The parameter sets the SEI parsers look at. buffering_period() names its SPS by id; pic_timing() and
/// decoded_picture_hash() use the SPS of the current picture, or for a prefix SEI before the first slice of the
/// access unit the SPS named by a buffering_period() of the same access unit, else the SPS of the previous picture.
struct SeiContext {
  std::span<const Vps* const> vps_by_id;  // indexed by the VPS id (0..15); null entries are unknown
  std::span<const Sps* const> sps_by_id;  // indexed by the SPS id (0..15); null entries are unknown
  const Sps* active_sps = nullptr;        // SPS of the current (suffix SEI) or previous (prefix SEI) picture
  /// Set by parse_sei_rbsp() from a buffering_period() of this access unit; the caller clears it at the next one.
  std::optional<uint32_t> buffering_period_sps_id;
};

struct SeiRbsp {
  bool suffix = false;  // nal_unit_type 40
  std::vector<SeiMessage> messages;
  bool complete = true;  // false when the framing stopped early (payloadSize beyond the RBSP)
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;  // set by the owning Unit
};

/// Annex D payload name of payloadType 0..205 (H.265 (08/2021) D.2.1); "reserved_sei_message" for an unassigned type.
[[nodiscard]] std::string_view sei_payload_name(uint32_t payload_type) noexcept;

/// True when D.2.1 allows the payload type in a prefix (`suffix == false`) or suffix SEI NAL unit.
[[nodiscard]] bool sei_payload_allowed(uint32_t payload_type, bool suffix) noexcept;

/// sei_rbsp() (§7.3.2.4) of a PREFIX_SEI_NUT or SUFFIX_SEI_NUT unit. Node `sei_rbsp`. Only a failure to read the
/// framing itself stops the message loop; it is not thrown but recorded (SeiRbsp::complete, Diagnostic).
SeiRbsp parse_sei_rbsp(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, SeiContext& ctx);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_SEI_HPP
