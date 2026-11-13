// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 profile_tier_level() (H.265 §7.3.3): typed struct and parser (chapter 7 §7.3.5 row 3, §7.3.7; WI-1.112).
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_PROFILE_TIER_LEVEL_HPP
#define OVT_CXX_SYNTAX_H265_PROFILE_TIER_LEVEL_HPP

#include <array>
#include <cstdint>
#include <string>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// The 88-bit profile block shared by the general and the sub-layer entries (H.265 §7.3.3). Field names drop the
/// `general_` / `sub_layer_` prefix. The constraint flags are decoded per profile (§7.4.4); a flag that the
/// profile does not signal stays 0.
struct ProfileInfo {
  uint8_t profile_space = 0;
  bool tier_flag = false;
  uint8_t profile_idc = 0;
  std::array<bool, 32> profile_compatibility_flag{};
  bool progressive_source_flag = false;
  bool interlaced_source_flag = false;
  bool non_packed_constraint_flag = false;
  bool frame_only_constraint_flag = false;
  // RExt / SCC / high throughput / multilayer profiles (profile_idc 4..11 or a compatibility flag of them)
  bool max_12bit_constraint_flag = false;
  bool max_10bit_constraint_flag = false;
  bool max_8bit_constraint_flag = false;
  bool max_422chroma_constraint_flag = false;
  bool max_420chroma_constraint_flag = false;
  bool max_monochrome_constraint_flag = false;
  bool intra_constraint_flag = false;
  bool one_picture_only_constraint_flag = false;  // also signalled by Main 10 (profile_idc 2)
  bool lower_bit_rate_constraint_flag = false;
  bool max_14bit_constraint_flag = false;  // profile_idc 5, 9, 10, 11
  uint64_t reserved_zero_bits = 0;         // the reserved 33/34/35/43 bits read as one value
  bool inbld_flag = false;                 // or reserved_zero_bit for the other profiles
  bool inbld_flag_present = false;
};

/// Typed profile_tier_level(profilePresentFlag, maxNumSubLayersMinus1) (§7.3.7).
struct ProfileTierLevel {
  bool profile_present_flag = false;
  uint8_t max_num_sub_layers_minus1 = 0;
  ProfileInfo general;
  uint8_t general_level_idc = 0;
  std::array<bool, 7> sub_layer_profile_present_flag{};
  std::array<bool, 7> sub_layer_level_present_flag{};
  std::array<ProfileInfo, 7> sub_layer;  // valid where sub_layer_profile_present_flag[i]
  std::array<uint8_t, 7> sub_layer_level_idc{};
  // derived (§7.3.5 row 3)
  std::string profile_name;  // "Main", "Main 10", "Main 4:4:4 12", "Screen-Extended Main", …
  double level = 0.0;        // general_level_idc / 30.0
  BitSpan span{0, 0};
};

/// Annex A profile name for a decoded profile block ("Main", "Main 10", "Main Still Picture", the RExt and SCC
/// sub-profiles by their constraint flags, "RExt (general_profile_idc 4)" when no row matches).
[[nodiscard]] std::string profile_name(const ProfileInfo& p);

/// Parses profile_tier_level(profilePresentFlag, maxNumSubLayersMinus1) into the node `profile_tier_level`.
/// maxNumSubLayersMinus1 must be <= 6 (the caller has range-checked the sub-layer count).
ProfileTierLevel parse_profile_tier_level(BitReader& br, SyntaxBuilder& b, bool profile_present_flag,
                                          unsigned max_num_sub_layers_minus1);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_PROFILE_TIER_LEVEL_HPP
