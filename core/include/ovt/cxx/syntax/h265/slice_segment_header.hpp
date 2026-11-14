// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 slice_segment_header(), ref_pic_lists_modification() and pred_weight_table() (H.265 §7.3.6): typed structs,
// parsers and the entry-point substream mapping (chapter 7 §7.3.5 rows 10–12, §7.3.7; WI-1.113). C++ helper header
// (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_SLICE_SEGMENT_HEADER_HPP
#define OVT_CXX_SYNTAX_H265_SLICE_SEGMENT_HEADER_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// slice_type values (H.265 Table 7-7).
enum SliceType : uint8_t { kSliceB = 0, kSliceP = 1, kSliceI = 2 };

/// "B", "P", "I" for 0..2, "reserved" otherwise.
[[nodiscard]] std::string_view slice_type_name(uint32_t slice_type) noexcept;

/// The PPS lookup a slice header activates through (§7.3.8): entry `id` is the PPS with pps_pic_parameter_set_id
/// `id` activated against its SPS, or nullptr when none was received.
// SPEC-ASSUMPTION(#391): §7.3.7 declares parse_slice_segment_header() with `const ActiveSets&`; the H.265
// ActiveSets is WI-1.115's, so the parser takes this table, which ActiveSets will own.
struct PpsTable {
  std::array<const ActivePps*, 64> entries{};
};

/// Upper bound of num_ref_idx_lX_active_minus1 + 1 (§7.4.7.1: num_ref_idx_lX_active_minus1 <= 14).
inline constexpr unsigned kMaxRefIdx = 15;
/// Upper bound of num_long_term_sps + num_long_term_pics kept in the struct (§7.4.7.1 bounds the sum by the DPB
/// size, <= 16; 32 leaves room for a non-conformant stream whose excess is reported, not stored).
inline constexpr unsigned kMaxLongTerm = 32;

/// ref_pic_lists_modification() (§7.3.6.2).
struct RefPicListsModification {
  bool ref_pic_list_modification_flag_l0 = false;
  std::array<uint32_t, kMaxRefIdx> list_entry_l0{};
  bool ref_pic_list_modification_flag_l1 = false;
  std::array<uint32_t, kMaxRefIdx> list_entry_l1{};
  BitSpan span{0, 0};
};

/// pred_weight_table() (§7.3.6.3) with the §7.4.7.3 derived weights. Index [i] is the reference index, [j] the
/// chroma component (0 Cb, 1 Cr).
struct PredWeightTable {
  struct List {
    std::array<bool, kMaxRefIdx> luma_weight_flag{};
    std::array<bool, kMaxRefIdx> chroma_weight_flag{};
    std::array<int32_t, kMaxRefIdx> delta_luma_weight{};
    std::array<int32_t, kMaxRefIdx> luma_offset{};
    std::array<std::array<int32_t, 2>, kMaxRefIdx> delta_chroma_weight{};
    std::array<std::array<int32_t, 2>, kMaxRefIdx> delta_chroma_offset{};
    // derived (7-56 and the LumaWeightLX / ChromaWeightLX semantics)
    std::array<int32_t, kMaxRefIdx> LumaWeight{};
    std::array<std::array<int32_t, 2>, kMaxRefIdx> ChromaWeight{};
    std::array<std::array<int32_t, 2>, kMaxRefIdx> ChromaOffset{};
  };
  uint32_t luma_log2_weight_denom = 0;
  int32_t delta_chroma_log2_weight_denom = 0;
  List l0;  // luma_weight_l0_flag[i] = l0.luma_weight_flag[i], …
  List l1;  // B slices only
  // derived
  uint32_t ChromaLog2WeightDenom = 0;
  BitSpan span{0, 0};
};

/// Typed slice_segment_header() (§7.3.7). For a dependent slice segment every element of the independent part
/// (slice_type … slice_loop_filter_across_slices_enabled_flag) and the derived values that depend on it are copied
/// from the preceding independent slice segment header (§7.4.7.1).
struct SliceSegmentHeader {  // NOLINT(clang-analyzer-optin.performance.Padding)
  bool first_slice_segment_in_pic_flag = false;
  bool no_output_of_prior_pics_flag = false;
  uint32_t slice_pic_parameter_set_id = 0;
  bool dependent_slice_segment_flag = false;
  uint32_t slice_segment_address = 0;
  std::array<bool, 7> slice_reserved_flag{};  // num_extra_slice_header_bits entries (u(3))
  uint32_t slice_type = kSliceI;
  bool pic_output_flag = true;  // inferred 1
  uint8_t colour_plane_id = 0;
  uint32_t slice_pic_order_cnt_lsb = 0;
  bool short_term_ref_pic_set_sps_flag = false;
  StRefPicSet st_ref_pic_set;  // the slice's own set (stRpsIdx = num_short_term_ref_pic_sets) when the flag is 0
  uint32_t short_term_ref_pic_set_idx = 0;
  uint32_t num_long_term_sps = 0;
  uint32_t num_long_term_pics = 0;
  std::array<uint32_t, kMaxLongTerm> lt_idx_sps{};
  std::array<uint32_t, kMaxLongTerm> poc_lsb_lt{};
  std::array<bool, kMaxLongTerm> used_by_curr_pic_lt_flag{};
  std::array<bool, kMaxLongTerm> delta_poc_msb_present_flag{};
  std::array<uint32_t, kMaxLongTerm> delta_poc_msb_cycle_lt{};
  bool slice_temporal_mvp_enabled_flag = false;
  bool slice_sao_luma_flag = false;
  bool slice_sao_chroma_flag = false;
  bool num_ref_idx_active_override_flag = false;
  uint32_t num_ref_idx_l0_active_minus1 = 0;  // PPS default unless overridden
  uint32_t num_ref_idx_l1_active_minus1 = 0;
  std::optional<RefPicListsModification> ref_pic_lists_modification;
  bool mvd_l1_zero_flag = false;
  bool cabac_init_flag = false;
  bool collocated_from_l0_flag = true;  // inferred 1
  uint32_t collocated_ref_idx = 0;
  std::optional<PredWeightTable> pred_weight_table;
  uint32_t five_minus_max_num_merge_cand = 0;
  bool use_integer_mv_flag = false;  // inferred from motion_vector_resolution_control_idc
  int32_t slice_qp_delta = 0;
  int32_t slice_cb_qp_offset = 0;
  int32_t slice_cr_qp_offset = 0;
  int32_t slice_act_y_qp_offset = 0;
  int32_t slice_act_cb_qp_offset = 0;
  int32_t slice_act_cr_qp_offset = 0;
  bool cu_chroma_qp_offset_enabled_flag = false;
  bool deblocking_filter_override_flag = false;
  bool slice_deblocking_filter_disabled_flag = false;         // inferred pps_deblocking_filter_disabled_flag
  int32_t slice_beta_offset_div2 = 0;                         // inferred pps_beta_offset_div2
  int32_t slice_tc_offset_div2 = 0;                           // inferred pps_tc_offset_div2
  bool slice_loop_filter_across_slices_enabled_flag = false;  // inferred pps_loop_filter_across_slices_enabled_flag
  uint32_t num_entry_point_offsets = 0;
  uint32_t offset_len_minus1 = 0;
  std::vector<uint32_t> entry_point_offset_minus1;
  uint32_t slice_segment_header_extension_length = 0;
  std::vector<uint8_t> slice_segment_header_extension_data_byte;

  // derived (§7.4.7.1, §7.4.7.2)
  uint32_t SliceAddrRs = 0;
  int32_t SliceQpY = 26;
  uint32_t MaxNumMergeCand = 5;
  uint32_t CurrRpsIdx = 0;                     // num_short_term_ref_pic_sets for the slice's own set
  uint32_t NumBitsForShortTermRPSInSlice = 0;  // bits of the slice's own st_ref_pic_set(), 0 otherwise
  uint32_t NumPicTotalCurr = 0;                // 7-55
  std::array<uint32_t, kMaxLongTerm> PocLsbLt{};
  std::array<bool, kMaxLongTerm> UsedByCurrPicLt{};
  std::array<uint64_t, kMaxLongTerm> DeltaPocMsbCycleLt{};  // 7-52 (cumulative)
  bool copied_from_independent = false;  // dependent segment whose independent part came from prev_independent
  /// Byte offset of the first slice_segment_data() byte in the RBSP the parser read (NAL header byte 0 = 0); map
  /// it to raw bytes with entry_point_substreams() or rbsp_bit_to_raw_bit().
  uint64_t slice_segment_data_rbsp_byte = 0;
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;  // set by the owning Unit
};

/// Parses ref_pic_lists_modification() into the node `ref_pic_lists_modification`. list_entry_lX[i] is u(v) with
/// v = Ceil(Log2(NumPicTotalCurr)); a value >= NumPicTotalCurr throws ParseError SYN-H265-0007.
RefPicListsModification parse_ref_pic_lists_modification(BitReader& br, SyntaxBuilder& b, uint32_t slice_type,
                                                         uint32_t num_ref_idx_l0_active_minus1,
                                                         uint32_t num_ref_idx_l1_active_minus1,
                                                         uint32_t num_pic_total_curr);

/// Parses pred_weight_table() into the node `pred_weight_table`. Weights and offsets are hard-bounded to their
/// §7.4.7.3 ranges (ParseError SYN-H265-0007) because they enter the weighted-prediction arithmetic. When
/// pps_curr_pic_ref_enabled_flag is set the current picture may be one of the references, whose flags the syntax
/// omits; RefPicList0/1 are not known here, so every entry is parsed as a non-current picture and Diagnostic
/// Warning SYN-H265-0040 is raised (§7.3.5 row 12).
PredWeightTable parse_pred_weight_table(BitReader& br, SyntaxBuilder& b, const Sps& sps, const Pps& pps,
                                        uint32_t slice_type, uint32_t num_ref_idx_l0_active_minus1,
                                        uint32_t num_ref_idx_l1_active_minus1);

/// Parses slice_segment_header() after the NAL unit header into the node `slice_segment_header`, followed by its
/// byte_alignment() and the opaque `slice_segment_data` remainder (§7.3.1 rule 2). slice_pic_parameter_set_id
/// selects `ppss.entries[id]`; an id above 63 or a missing PPS throws ParseError. `prev_independent` is the
/// preceding independent slice segment header of the same picture; a dependent segment copies its independent part
/// from it (Diagnostic Error when it is nullptr, and the independent part keeps its inferred defaults).
SliceSegmentHeader parse_slice_segment_header(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                              const PpsTable& ppss, const SliceSegmentHeader* prev_independent);

/// The short-term RPS the slice uses: its own set, the SPS set short_term_ref_pic_set_idx, or nullptr for IDR
/// pictures (no RPS is signalled) and an SPS without that set.
[[nodiscard]] const StRefPicSet* current_st_ref_pic_set(const SliceSegmentHeader& sh, const Sps& sps) noexcept;

/// One substream of slice_segment_data() (§7.4.7.1): bytes [first_byte, first_byte + size) of the NAL unit in raw
/// bytes, emulation prevention bytes included (NAL header byte 0 = 0, the convention of Rbsp::epb_positions).
struct Substream {
  uint64_t first_byte = 0;
  uint64_t size = 0;
};

/// The num_entry_point_offsets + 1 substreams of a slice segment NAL unit of `nal_size` raw bytes:
/// firstByte[k] = sum of (entry_point_offset_minus1[n] + 1) for n < k, counted from the first raw byte of
/// slice_segment_data(), and the last substream runs to the end of the unit. `epb_positions` are those of the unit
/// (Rbsp::epb_positions). Returns std::nullopt when the offsets overrun the unit.
[[nodiscard]] std::optional<std::vector<Substream>> entry_point_substreams(const SliceSegmentHeader& sh,
                                                                           std::span<const uint32_t> epb_positions,
                                                                           uint64_t nal_size);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_SLICE_SEGMENT_HEADER_HPP
