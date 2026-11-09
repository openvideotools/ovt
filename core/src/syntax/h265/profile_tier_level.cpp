// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 profile_tier_level() parser (H.265 §7.3.3, §7.4.4, Annex A; chapter 7 §7.3.5 row 3, WI-1.112). The
// constraint-flag block after general_frame_only_constraint_flag is laid out per profile exactly as the syntax
// table: 9 flags + (max_14bit + 33 reserved | 34 reserved) for the range-extension family, 7 reserved +
// one_picture_only + 35 reserved for Main 10, 43 reserved otherwise; then inbld_flag or a reserved bit.

#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// Profile `idc` or any compatibility flag of a profile in `idcs` (the "general_profile_idc == X ||
// general_profile_compatibility_flag[X]" conditions of §7.3.3).
template <size_t N>
bool profile_in(const ProfileInfo& p, const std::array<uint8_t, N>& idcs) {
  return std::ranges::any_of(idcs,
                             [&p](uint8_t idc) { return p.profile_idc == idc || p.profile_compatibility_flag[idc]; });
}

constexpr std::array<uint8_t, 8> kRangeFamily = {4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<uint8_t, 4> kHas14bitFlag = {5, 9, 10, 11};
constexpr std::array<uint8_t, 1> kMain10 = {2};
constexpr std::array<uint8_t, 7> kHasInbld = {1, 2, 3, 4, 5, 9, 11};

// Reads the 88-bit profile block. `pfx` is "general_" or "sub_layer_".
ProfileInfo read_profile_block(SyntaxBuilder& b, std::string_view pfx) {
  const auto name = [pfx](std::string_view suffix) { return fmt::format("{}{}", pfx, suffix); };
  ProfileInfo p;
  p.profile_space = static_cast<uint8_t>(b.read_u(name("profile_space"), 2));
  p.tier_flag = b.read_flag(name("tier_flag"));
  p.profile_idc = static_cast<uint8_t>(b.read_u(name("profile_idc"), 5));
  const std::string compat = name("profile_compatibility_flag");
  for (int j = 0; j < 32; ++j) {
    p.profile_compatibility_flag[static_cast<size_t>(j)] = detail::read_flag_at(b, compat, j);
  }
  p.progressive_source_flag = b.read_flag(name("progressive_source_flag"));
  p.interlaced_source_flag = b.read_flag(name("interlaced_source_flag"));
  p.non_packed_constraint_flag = b.read_flag(name("non_packed_constraint_flag"));
  p.frame_only_constraint_flag = b.read_flag(name("frame_only_constraint_flag"));
  if (profile_in(p, kRangeFamily)) {
    p.max_12bit_constraint_flag = b.read_flag(name("max_12bit_constraint_flag"));
    p.max_10bit_constraint_flag = b.read_flag(name("max_10bit_constraint_flag"));
    p.max_8bit_constraint_flag = b.read_flag(name("max_8bit_constraint_flag"));
    p.max_422chroma_constraint_flag = b.read_flag(name("max_422chroma_constraint_flag"));
    p.max_420chroma_constraint_flag = b.read_flag(name("max_420chroma_constraint_flag"));
    p.max_monochrome_constraint_flag = b.read_flag(name("max_monochrome_constraint_flag"));
    p.intra_constraint_flag = b.read_flag(name("intra_constraint_flag"));
    p.one_picture_only_constraint_flag = b.read_flag(name("one_picture_only_constraint_flag"));
    p.lower_bit_rate_constraint_flag = b.read_flag(name("lower_bit_rate_constraint_flag"));
    if (profile_in(p, kHas14bitFlag)) {
      p.max_14bit_constraint_flag = b.read_flag(name("max_14bit_constraint_flag"));
      p.reserved_zero_bits = b.read_f(name("reserved_zero_33bits"), 33, 0);
    } else {
      p.reserved_zero_bits = b.read_f(name("reserved_zero_34bits"), 34, 0);
    }
  } else if (profile_in(p, kMain10)) {
    p.reserved_zero_bits = b.read_f(name("reserved_zero_7bits"), 7, 0);
    p.one_picture_only_constraint_flag = b.read_flag(name("one_picture_only_constraint_flag"));
    p.reserved_zero_bits = (p.reserved_zero_bits << 35) | b.read_f(name("reserved_zero_35bits"), 35, 0);
  } else {
    p.reserved_zero_bits = b.read_f(name("reserved_zero_43bits"), 43, 0);
  }
  if (profile_in(p, kHasInbld)) {
    p.inbld_flag = b.read_flag(name("inbld_flag"));
    p.inbld_flag_present = true;
  } else {
    (void)b.read_f(name("reserved_zero_bit"), 1, 0);
  }
  return p;
}

// One row of the RExt (Table A.2) / SCC (Table A.5) constraint-flag tables: the nine flags in syntax order,
// '1'/'0' fixed or 'x' don't-care.
struct ConstraintRow {
  std::string_view flags;  // 9 characters: 12bit 10bit 8bit 422 420 mono intra one_pic lower_bit_rate
  std::string_view name;
};

constexpr std::array<ConstraintRow, 22> kRextRows = {{
    {"111111001", "Monochrome"},
    {"110111001", "Monochrome 10"},
    {"100111001", "Monochrome 12"},
    {"000111001", "Monochrome 16"},
    {"100110001", "Main 12"},
    {"110100001", "Main 4:2:2 10"},
    {"100100001", "Main 4:2:2 12"},
    {"111000001", "Main 4:4:4"},
    {"110000001", "Main 4:4:4 10"},
    {"100000001", "Main 4:4:4 12"},
    {"11111010x", "Main Intra"},
    {"11011010x", "Main 10 Intra"},
    {"10011010x", "Main 12 Intra"},
    {"11010010x", "Main 4:2:2 10 Intra"},
    {"10010010x", "Main 4:2:2 12 Intra"},
    {"11100010x", "Main 4:4:4 Intra"},
    {"11000010x", "Main 4:4:4 10 Intra"},
    {"10000010x", "Main 4:4:4 12 Intra"},
    {"00000010x", "Main 4:4:4 16 Intra"},
    {"11100011x", "Main 4:4:4 Still Picture"},
    {"00000011x", "Main 4:4:4 16 Still Picture"},
    {"xxxxxxxxx", "RExt (general_profile_idc 4)"},
}};

constexpr std::array<ConstraintRow, 5> kSccRows = {{
    {"111110001", "Screen-Extended Main"},
    {"110110001", "Screen-Extended Main 10"},
    {"111000001", "Screen-Extended Main 4:4:4"},
    {"110000001", "Screen-Extended Main 4:4:4 10"},
    {"xxxxxxxxx", "SCC (general_profile_idc 9)"},
}};

template <size_t N>
std::string match_row(const ProfileInfo& p, const std::array<ConstraintRow, N>& rows) {
  const std::array<bool, 9> flags = {
      p.max_12bit_constraint_flag,     p.max_10bit_constraint_flag,        p.max_8bit_constraint_flag,
      p.max_422chroma_constraint_flag, p.max_420chroma_constraint_flag,    p.max_monochrome_constraint_flag,
      p.intra_constraint_flag,         p.one_picture_only_constraint_flag, p.lower_bit_rate_constraint_flag};
  for (const ConstraintRow& row : rows) {
    bool ok = true;
    for (size_t i = 0; i < flags.size() && ok; ++i) {
      const char c = row.flags[i];
      ok = c == 'x' || (c == '1') == flags[i];
    }
    if (ok) {
      return std::string(row.name);
    }
  }
  return std::string(rows.back().name);
}

// Name of profile `idc` with the constraint flags of `p` (Annex A); empty when idc names no profile.
std::string name_for_idc(const ProfileInfo& p, uint8_t idc) {
  switch (idc) {
    case 1:
      return "Main";
    case 2:
      return p.one_picture_only_constraint_flag ? "Main 10 Still Picture" : "Main 10";
    case 3:
      return "Main Still Picture";
    case 4:
      return match_row(p, kRextRows);
    case 5:
      return "High Throughput 4:4:4";
    case 6:
      return "Multiview Main";
    case 7:
      return "Scalable Main";
    case 8:
      return "3D Main";
    case 9:
      return match_row(p, kSccRows);
    case 10:
      return "High Throughput Screen-Extended 4:4:4";
    case 11:
      return "Scalable Range Extensions";
    default:
      return {};
  }
}

}  // namespace

std::string profile_name(const ProfileInfo& p) {
  if (std::string name = name_for_idc(p, p.profile_idc); !name.empty()) {
    return name;
  }
  // profile_idc 0 with a compatibility flag set names the compatible profile (A.3: decoders use the flags).
  for (uint8_t idc = 1; idc <= 11; ++idc) {
    if (p.profile_compatibility_flag[idc]) {
      return name_for_idc(p, idc);
    }
  }
  return fmt::format("unknown (general_profile_idc {})", p.profile_idc);
}

ProfileTierLevel parse_profile_tier_level(BitReader& br, SyntaxBuilder& b, bool profile_present_flag,
                                          unsigned max_num_sub_layers_minus1) {
  detail::require_range(br, "maxNumSubLayersMinus1", max_num_sub_layers_minus1, 0, 6);
  ProfileTierLevel ptl;
  ptl.profile_present_flag = profile_present_flag;
  ptl.max_num_sub_layers_minus1 = static_cast<uint8_t>(max_num_sub_layers_minus1);
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("profile_tier_level", "H.265 7.3.3");
  if (profile_present_flag) {
    ptl.general = read_profile_block(b, "general_");
  }
  ptl.general_level_idc = static_cast<uint8_t>(b.read_u("general_level_idc", 8));
  for (unsigned i = 0; i < max_num_sub_layers_minus1; ++i) {
    ptl.sub_layer_profile_present_flag[i] =
        detail::read_flag_at(b, "sub_layer_profile_present_flag", static_cast<int>(i));
    ptl.sub_layer_level_present_flag[i] = detail::read_flag_at(b, "sub_layer_level_present_flag", static_cast<int>(i));
  }
  if (max_num_sub_layers_minus1 > 0) {
    for (unsigned i = max_num_sub_layers_minus1; i < 8; ++i) {
      b.begin_array_element("reserved_zero_2bits", static_cast<int>(i));
      (void)b.read_f("reserved_zero_2bits", 2, 0);
      b.end_array_element();
    }
  }
  for (unsigned i = 0; i < max_num_sub_layers_minus1; ++i) {
    if (ptl.sub_layer_profile_present_flag[i]) {
      b.begin_array_element("sub_layer_profile", static_cast<int>(i));
      ptl.sub_layer[i] = read_profile_block(b, "sub_layer_");
      b.end_array_element();
    }
    if (ptl.sub_layer_level_present_flag[i]) {
      ptl.sub_layer_level_idc[i] =
          static_cast<uint8_t>(detail::read_u_at(b, "sub_layer_level_idc", static_cast<int>(i), 8));
    }
  }
  if (profile_present_flag) {
    ptl.profile_name = profile_name(ptl.general);
    b.derived("profile_name", std::string_view{ptl.profile_name});
  }
  ptl.level = ptl.general_level_idc / 30.0;
  b.derived("level", std::string_view{fmt::format("level = {:g}", ptl.level)});
  ptl.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return ptl;
}

}  // namespace ovt::syntax::h265
