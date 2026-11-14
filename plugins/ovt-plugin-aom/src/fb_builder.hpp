// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// FrameAnalysis FlatBuffer of one AV1 frame (chapter 07 §7.6.1 mapping; WI-2.155, block data WI-2.156,
// planes WI-2.158).
#ifndef OVT_PLUGIN_AOM_FB_BUILDER_HPP
#define OVT_PLUGIN_AOM_FB_BUILDER_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "decoder.hpp"
#include "planes.hpp"

namespace ovt::aom {

/// Builds the FrameAnalysis of @p f: FrameInfo, the cell grid, block tree, motion vectors, reference slots, tiles
/// and Av1FilterParams mapped from its BlockData (map_mi.hpp), the Reconstruction PlaneRefs when
/// @p plane_stage_mask asks for them (planes.hpp), and diagnostics. Each PlaneRef's blob id is added to @p blobs
/// with a view into @p f's Picture, which must outlive the map.
std::unique_ptr<flatbuffers::FlatBufferBuilder> build_frame_analysis(const Frame& f, uint32_t plane_stage_mask,
                                                                     const char* plugin_id, const char* plugin_version,
                                                                     std::map<std::string, PlaneView>* blobs);

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_FB_BUILDER_HPP
