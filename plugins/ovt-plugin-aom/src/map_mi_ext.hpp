// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Mapping of the fields that libaom extension patch 0001 adds to the inspection data (chapter 07 §7.6.1 extension
// table, WI-2.159): Av1Block mode-info fields, the corrected tx_type, exact cdef_idx, MotionVector.mvd and the
// frame-level loop filter and loop restoration parameters of Av1FilterParams. map_mi.cpp calls these only when
// BlockData::ext is set. Free of libaom headers, like map_mi.hpp.
#ifndef OVT_PLUGIN_AOM_MAP_MI_EXT_HPP
#define OVT_PLUGIN_AOM_MAP_MI_EXT_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <vector>

#include "block_data.hpp"
#include "ovt_model_generated.h"

namespace ovt::aom {

/// Grid fields the extension adds to map_mi.hpp's kGridFields: tx_type (bit 11).
constexpr uint32_t kGridFieldsExt = 1U << 11U;

/// The Av1Block vectors of the extension fields of one leaf block. They must exist before the Av1BlockBuilder of
/// that block starts, so make_block_ext() runs first and add_block_ext() fills the builder.
struct Av1BlockExt {
  flatbuffers::Offset<flatbuffers::Vector<int8_t>> delta_lf;
  flatbuffers::Offset<flatbuffers::Vector<int16_t>> mvd;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> mv_joint;
  flatbuffers::Offset<flatbuffers::Vector<int32_t>> warp;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> palette;  // nested Av1Palette
};

/// Creates the vectors of @p m (a leaf block) in @p fbb.
Av1BlockExt make_block_ext(flatbuffers::FlatBufferBuilder& fbb, const MiInfo& m, bool inter, bool delta_lf_present);

/// Adds the extension fields of leaf @p m to @p b, with the vectors @p v from make_block_ext(). @p inter: an inter
/// block (not IntraBC).
void add_block_ext(ovt::fb::Av1BlockBuilder& b, const MiInfo& m, const Av1BlockExt& v, bool inter);

/// grid.tx_type of one cell: TX_TYPE 0..15, 0xFF for skipped blocks or when unknown.
uint8_t grid_tx_type(const MiInfo& m);

/// MotionVector.mvd of reference list @p list in 1/16 luma samples (×2 of libaom's 1/8), or -32768 for both
/// components when that list did not code a difference (no NEWMV).
struct Mvd16 {
  int16_t x = -32768;
  int16_t y = -32768;
};
Mvd16 mvd_of(const MiInfo& m, int list);

/// cdef_idx of the 64x64 CDEF unit whose origin MI is @p origin, when the unit was coded: libaom's own index.
/// 0xFF when the value is out of range for the frame's strength table of @p num_strengths entries.
uint8_t ext_cdef_idx(const MiInfo& origin, size_t num_strengths);

/// The Av1FilterParams vectors of the extension's frame-level data; like Av1BlockExt, made before the builder.
struct FilterParamsExt {
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> lf_level;
  flatbuffers::Offset<flatbuffers::Vector<int8_t>> ref_deltas;
  flatbuffers::Offset<flatbuffers::Vector<int8_t>> mode_deltas;
  flatbuffers::Offset<flatbuffers::Vector<ovt::fb::Av1RestorationType>> lr_type;
  flatbuffers::Offset<flatbuffers::Vector<uint16_t>> lr_size;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::Av1LrUnit>>> lr_units;
};

/// Creates the vectors of @p d.filters in @p fbb (the nested Av1FilterParams builder). Loop restoration unit
/// rectangles are in luma samples of the upscaled frame (UpscaledWidth x @p frame_h).
FilterParamsExt make_filter_ext(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, int32_t frame_h);

/// Adds the frame-level extension fields of @p d.filters to @p p.
void add_filter_ext(ovt::fb::Av1FilterParamsBuilder& p, const BlockData& d, const FilterParamsExt& v);

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_MAP_MI_EXT_HPP
