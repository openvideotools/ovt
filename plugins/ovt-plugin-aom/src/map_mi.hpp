// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// insp_mi_data -> CellGrid / Block tree / MotionVector / RefSlot (chapter 07 §7.6.1 mapping table, WI-2.156).
// map_blocks() turns the BlockData that the inspection callback copied (decoder.cpp) into FlatBuffers parts of
// the FrameAnalysis, on the host's thread. Free of libaom headers so that the unit tests can build BlockData by hand.
#ifndef OVT_PLUGIN_AOM_MAP_MI_HPP
#define OVT_PLUGIN_AOM_MAP_MI_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>
#include <vector>

#include "block_data.hpp"
#include "ovt_model_generated.h"

namespace ovt::aom {

/// The FlatBuffers parts map_blocks() writes into the caller's builder.
struct MappedBlocks {
  flatbuffers::Offset<ovt::fb::CellGrid> grid;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::Block>>> blocks;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::MotionVector>>> mvs;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::RefSlot>>> refs;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::PartitionInfo>>> partitions;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> filter_params;                             // nested Av1FilterParams
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ovt::fb::SymbolCost>>> symbols;  // WI-2.157
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> symbol_names;
  std::vector<std::string> warnings;  // messages for Warning Diagnostics on the frame
  struct Note {
    std::string code;
    std::string message;
  };
  std::vector<Note> notes;  // Info Diagnostics on the frame
};

/// Grid fields map_blocks() fills from insp_mi_data (ovt_AnalysisCaps.grid_fields, chapter 07 §7.6.1 Caps row):
/// everything except tx_type (stock ifd_inspect() writes 0 for coded blocks), bits_x8 (from accounting:
/// kGridFieldBits of map_accounting.hpp, added when libaom records it) and deblock_bs.
constexpr uint32_t kGridFields = 0x9FFFU & ~(1U << 11);

/// Maps @p d for a frame of @p coded_w x @p coded_h luma samples (FrameWidth x FrameHeight) into @p fbb. Must not
/// be called while another table is being built in @p fbb. With `d.valid == false` only the empty grid is made.
MappedBlocks map_blocks(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, int32_t coded_w, int32_t coded_h);

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_MAP_MI_HPP
