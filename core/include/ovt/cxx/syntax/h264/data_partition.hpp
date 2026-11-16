// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 slice_data_partition_a_layer_rbsp(), slice_data_partition_b_layer_rbsp() and
// slice_data_partition_c_layer_rbsp() (§7.3.2.9): chapter 7 §7.3.4 row 28 and §7.3.7, WI-1.109. C++ helper header
// (D22): semi-public, no ABI promise.

#ifndef OVT_CXX_SYNTAX_H264_DATA_PARTITION_HPP
#define OVT_CXX_SYNTAX_H264_DATA_PARTITION_HPP

#include <cstdint>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// slice_data_partition_a_layer_rbsp() (nal_unit_type 2).
struct SliceDataPartitionA {
  SliceHeader slice_header;
  uint32_t slice_id = 0;
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// slice_data_partition_b_layer_rbsp() / slice_data_partition_c_layer_rbsp() (nal_unit_type 3 / 4).
struct SliceDataPartitionBC {
  bool partition_c = false;
  uint32_t slice_id = 0;
  uint32_t colour_plane_id = 0;    // separate_colour_plane_flag only
  uint32_t redundant_pic_cnt = 0;  // redundant_pic_cnt_present_flag only
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;
};

/// Row 28, partition A: slice_header() (activating its PPS and SPS through `sets`), slice_id, then the opaque
/// `slice_data`. Node `slice_data_partition_a_layer_rbsp`. The reader stands after the NAL header.
SliceDataPartitionA parse_slice_data_partition_a(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                                 const ParameterSets& sets);

/// Row 28, partitions B and C (nal_unit_type 3 or 4, from `nal`): slice_id, colour_plane_id, redundant_pic_cnt,
/// then the opaque `slice_data`. `sps` / `pps` are the sets the partition A with the same slice_id activated.
/// Node `slice_data_partition_b_layer_rbsp` / `slice_data_partition_c_layer_rbsp`.
SliceDataPartitionBC parse_slice_data_partition_bc(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                                   const Sps& sps, const Pps& pps);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_DATA_PARTITION_HPP
