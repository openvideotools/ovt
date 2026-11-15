// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 slice_header() (§7.3.3), ref_pic_list_modification() / ref_pic_list_mvc_modification() (§7.3.3.1,
// H.7.3.3.1.1), pred_weight_table() (§7.3.3.2) and dec_ref_pic_marking() (§7.3.3.3) with their derived variables:
// chapter 7 §7.3.4 rows 24–27 and §7.3.7, WI-1.109. C++ helper header (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_SLICE_HEADER_HPP
#define OVT_CXX_SYNTAX_H264_SLICE_HEADER_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// slice_type % 5 (Table 7-6).
enum SliceType : uint8_t { kSliceP = 0, kSliceB = 1, kSliceI = 2, kSliceSP = 3, kSliceSI = 4 };

/// "P", "B", "I", "SP", "SI" for slice_type 0..9 (Table 7-6), "reserved" otherwise.
[[nodiscard]] std::string_view slice_type_name(uint32_t slice_type) noexcept;

/// Upper bound of num_ref_idx_lX_active_minus1 + 1 (§7.4.3: num_ref_idx_lX_active_minus1 <= 31 for fields).
inline constexpr unsigned kMaxRefIdx = 32;
/// Stored modification operations per list: num_ref_idx_lX_active_minus1 + 1 at most (§7.4.3.1). Operations beyond
/// are read and reported (Diagnostic SYN-H264-0002) but not stored.
inline constexpr unsigned kMaxModificationOps = kMaxRefIdx;
/// Stored memory_management_control_operation entries (§7.3.4 row 27: 66 at most). Excess entries are read and
/// reported (Diagnostic SYN-H264-0002) but not stored.
inline constexpr unsigned kMaxMmcoOps = 66;

/// The PPS lookup a slice header activates through (§7.3.8): entry `id` is the latest received PPS with
/// pic_parameter_set_id `id`, or nullptr. Mirrors SpsTable.
struct PpsTable {
  std::array<const Pps*, 256> entries{};
  [[nodiscard]] const Pps* find(uint32_t pic_parameter_set_id) const noexcept {
    return pic_parameter_set_id < entries.size() ? entries[pic_parameter_set_id] : nullptr;
  }
};

/// The parameter sets parse_slice_header() resolves pic_parameter_set_id through (§7.3.7, §7.3.8; #529): WI-1.111's
/// ActiveSets owns one and passes it, and records the activation through the optional on_activate callback.
struct ParameterSets {
  PpsTable pps;
  SpsTable sps;         // nal_unit_type 1, 5 (and 2 for partition A)
  SpsTable subset_sps;  // nal_unit_type 20, 21 (H.7.4.1.2.1): seq_parameter_set_data() of the subset SPS
  /// Called after pic_parameter_set_id is read and its PPS and SPS were found, before any element that depends on
  /// them is read. May be empty.
  std::function<void(const NalHeader&, const Pps&, const Sps&)> on_activate;
};

/// One modification_of_pic_nums_idc entry (§7.3.3.1, H.7.3.3.1.1). The terminating idc 3 is not stored.
struct RefPicListModificationOp {
  uint32_t modification_of_pic_nums_idc = 0;  // 0..2; 4..5 for ref_pic_list_mvc_modification()
  uint32_t abs_diff_pic_num_minus1 = 0;       // idc 0, 1
  uint32_t long_term_pic_num = 0;             // idc 2
  uint32_t abs_diff_view_idx_minus1 = 0;      // idc 4, 5 (MVC)
};

/// ref_pic_list_modification() (§7.3.3.1) or ref_pic_list_mvc_modification() (H.7.3.3.1.1), row 25. The ordered
/// op lists are what the reference-list modification process (§8.2.4.3, chapter 7 §7.4.4) consumes.
struct RefPicListModification {
  bool mvc = false;  // ref_pic_list_mvc_modification() (nal_unit_type 20 or 21)
  bool ref_pic_list_modification_flag_l0 = false;
  std::vector<RefPicListModificationOp> ops_l0;
  bool ref_pic_list_modification_flag_l1 = false;
  std::vector<RefPicListModificationOp> ops_l1;
  BitSpan span{0, 0};
};

/// pred_weight_table() (§7.3.3.2), row 26. Index [i] is the reference index, [j] the chroma component (0 Cb, 1 Cr).
/// The weight and offset arrays hold the inferred values (§7.4.3.2) when their flag is 0: weight = 2^denom,
/// offset = 0.
struct PredWeightTable {
  struct List {
    std::array<bool, kMaxRefIdx> luma_weight_flag{};
    std::array<int32_t, kMaxRefIdx> luma_weight{};
    std::array<int32_t, kMaxRefIdx> luma_offset{};
    std::array<bool, kMaxRefIdx> chroma_weight_flag{};
    std::array<std::array<int32_t, 2>, kMaxRefIdx> chroma_weight{};
    std::array<std::array<int32_t, 2>, kMaxRefIdx> chroma_offset{};
  };
  uint32_t luma_log2_weight_denom = 0;
  uint32_t chroma_log2_weight_denom = 0;  // read only when ChromaArrayType != 0
  List l0;                                // luma_weight_l0_flag[i] = l0.luma_weight_flag[i], …
  List l1;                                // B slices only
  BitSpan span{0, 0};
};

/// One memory_management_control_operation (§7.3.3.3). The terminating operation 0 is not stored.
struct MmcoOp {
  uint32_t memory_management_control_operation = 0;  // 1..6
  uint32_t difference_of_pic_nums_minus1 = 0;        // ops 1, 3
  uint32_t long_term_pic_num = 0;                    // op 2
  uint32_t long_term_frame_idx = 0;                  // ops 3, 6
  uint32_t max_long_term_frame_idx_plus1 = 0;        // op 4
};

/// MaxLongTermFrameIdx value "no long-term frame indices" (§7.4.3.3, §8.2.5.1).
inline constexpr int32_t kNoLongTermFrameIndices = -1;

/// dec_ref_pic_marking() (§7.3.3.3), row 27.
struct DecRefPicMarking {
  bool idr = false;  // IdrPicFlag: the first branch was read
  bool no_output_of_prior_pics_flag = false;
  bool long_term_reference_flag = false;
  bool adaptive_ref_pic_marking_mode_flag = false;
  std::vector<MmcoOp> mmco;  // in bitstream order
  // derived
  bool has_mmco5 = false;  // memory_management_control_operation 5 present (§8.2.1: POC / frame_num reset)
  /// MaxLongTermFrameIdx after this picture's marking (§8.2.5.1, §8.2.5.4.4): IDR → 0 (long_term_reference_flag)
  /// or kNoLongTermFrameIndices; the last op 4 → max_long_term_frame_idx_plus1 − 1; op 5 → kNoLongTermFrameIndices;
  /// nullopt when the picture leaves it unchanged.
  std::optional<int32_t> MaxLongTermFrameIdx;
  BitSpan span{0, 0};
};

/// PictureStructure of the slice (chapter 6 FrameInfo.structure).
enum class PictureStructure : uint8_t { Frame = 0, TopField = 1, BottomField = 2, Mbaff = 3 };

/// Typed slice_header() (§7.3.3), row 24. Fields follow the syntax-table order, not the padding-optimal one.
struct SliceHeader {  // NOLINT(clang-analyzer-optin.performance.Padding)
  uint32_t first_mb_in_slice = 0;
  uint32_t slice_type = 0;
  uint32_t pic_parameter_set_id = 0;
  uint32_t colour_plane_id = 0;
  uint32_t frame_num = 0;
  bool field_pic_flag = false;
  bool bottom_field_flag = false;
  uint32_t idr_pic_id = 0;
  uint32_t pic_order_cnt_lsb = 0;
  int32_t delta_pic_order_cnt_bottom = 0;
  std::array<int32_t, 2> delta_pic_order_cnt{};
  uint32_t redundant_pic_cnt = 0;
  bool direct_spatial_mv_pred_flag = false;
  bool num_ref_idx_active_override_flag = false;
  uint32_t num_ref_idx_l0_active_minus1 = 0;  // the PPS default when not overridden (§7.4.3)
  uint32_t num_ref_idx_l1_active_minus1 = 0;
  RefPicListModification ref_pic_list_modification;
  bool has_pred_weight_table = false;
  PredWeightTable pred_weight_table;
  bool has_dec_ref_pic_marking = false;  // nal_ref_idc != 0
  DecRefPicMarking dec_ref_pic_marking;
  uint32_t cabac_init_idc = 0;
  int32_t slice_qp_delta = 0;
  bool sp_for_switch_flag = false;
  int32_t slice_qs_delta = 0;
  uint32_t disable_deblocking_filter_idc = 0;
  int32_t slice_alpha_c0_offset_div2 = 0;
  int32_t slice_beta_offset_div2 = 0;
  uint32_t slice_group_change_cycle = 0;
  // derived (§7.4.3)
  uint32_t seq_parameter_set_id = 0;  // of the activated SPS
  bool uses_subset_sps = false;       // nal_unit_type 20 / 21: activated through ParameterSets::subset_sps
  SliceType slice_type_mod5 = kSliceP;
  std::string_view slice_type_name;  // static storage
  bool IdrPicFlag = false;           // nal_unit_type 5, or !non_idr_flag of the MVC / 3D-AVC header
  bool MbaffFrameFlag = false;
  uint32_t PicHeightInMbs = 1;
  uint64_t PicSizeInMbs = 1;  // 64-bit like Sps::PicSizeInMapUnits: up to 2^32 under the #388 caps
  uint32_t MaxPicNum = 16;
  uint32_t CurrPicNum = 0;
  int32_t SliceQPY = 26;
  int32_t QSY = 26;  // SP / SI slices only
  int32_t FilterOffsetA = 0;
  int32_t FilterOffsetB = 0;
  uint32_t slice_group_change_cycle_bits =
      0;  // the u(v) width, Ceil(Log2(PicSizeInMapUnits ÷ SliceGroupChangeRate + 1))
  PictureStructure structure = PictureStructure::Frame;
  uint64_t header_bits = 0;  // bit offset of slice_data() in the RBSP (the NAL header included)
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 25. Node `ref_pic_list_modification` (`ref_pic_list_mvc_modification` when `mvc`). Throws ParseError when an
/// idc is out of range.
RefPicListModification parse_ref_pic_list_modification(BitReader& br, SyntaxBuilder& b, uint32_t slice_type,
                                                       uint32_t num_ref_idx_l0_active_minus1,
                                                       uint32_t num_ref_idx_l1_active_minus1, bool mvc);

/// Row 26. Node `pred_weight_table`. `chroma_array_type` is the active SPS's ChromaArrayType.
PredWeightTable parse_pred_weight_table(BitReader& br, SyntaxBuilder& b, uint32_t slice_type,
                                        uint32_t chroma_array_type, uint32_t num_ref_idx_l0_active_minus1,
                                        uint32_t num_ref_idx_l1_active_minus1);

/// Row 27. Node `dec_ref_pic_marking`. `max_num_ref_frames` bounds max_long_term_frame_idx_plus1 (a Warning).
DecRefPicMarking parse_dec_ref_pic_marking(BitReader& br, SyntaxBuilder& b, bool idr_pic_flag,
                                           uint32_t max_num_ref_frames);

/// Row 24: slice_header() for nal_unit_type 1, 5 and 2 (partition A), and for 20 / 21 with the MVC extension
/// header (H.7.3.3.4: the same syntax with the subset SPS as the active SPS). The reader stands after the NAL
/// header; the RBSP remainder is recorded as the opaque `slice_data` node by the caller (parse_slice_layer()).
/// Throws ParseError SYN-H264-0010 when the PPS or its (subset) SPS is missing, SYN-H264-0002 on a range violation
/// and for an SVC / 3D-AVC enhancement-layer slice (WI-4.111, #529).
SliceHeader parse_slice_header(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, const ParameterSets& sets);

/// slice_layer_without_partitioning_rbsp() (§7.3.2.8) and slice_layer_extension_rbsp() (§7.3.2.13): node
/// `slice_layer_without_partitioning_rbsp` / `slice_layer_extension_rbsp` holding `slice_header` and the opaque
/// `slice_data`. A NAL 20 unit with svc_extension_flag = 1 or a NAL 21 unit with avc_3d_extension_flag = 1 is kept
/// as the opaque `slice_layer_extension_bytes` with Info SYN-H264-0070 and returns std::nullopt: its
/// slice_header_in_scalable_extension() is WI-4.111's (§7.9 table, #529).
std::optional<SliceHeader> parse_slice_layer(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                             const ParameterSets& sets);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SLICE_HEADER_HPP
