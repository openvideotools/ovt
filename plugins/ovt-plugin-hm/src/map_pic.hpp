// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// The picture-level data of one HM picture (chapter 07 §7.6.4, WI-4.105 part c): PartitionInfo per slice segment
// and tile, HevcFilterParams and HevcRps. Used by map_cu.cpp, which also needs the CTU -> partition maps.
#ifndef OVT_PLUGIN_HM_MAP_PIC_HPP
#define OVT_PLUGIN_HM_MAP_PIC_HPP

#include <cstdint>
#include <vector>

#include "analysis.hpp"

namespace ovt::hm {

/// Per CTU (raster address): its slice segment's PartitionInfo index, its tile's (65535 with one tile), and whether
/// it is the last CTU of its slice segment.
struct PicMaps {
  std::vector<uint16_t> segment;
  std::vector<uint16_t> tile;
  std::vector<uint8_t> last_in_segment;
};

/// Fills out->partitions, out->filter_params and out->ref_state from @p pic's slices and returns the CTU maps.
PicMaps map_pic(TComPic& pic, const PicIndex& index, int dpb_fullness, Analysis* out);

}  // namespace ovt::hm

#endif  // OVT_PLUGIN_HM_MAP_PIC_HPP
