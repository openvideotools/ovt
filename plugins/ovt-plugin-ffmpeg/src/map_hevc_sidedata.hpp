// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The hevc decoder's side data -> OVT model, for the optional ffmpeg_hevc plugin (chapter 07 §7.8 WI-4.107). Patch 0002
// (plugins/ovt-plugin-ffmpeg/patches) makes libavcodec's hevc decoder export AV_FRAME_DATA_VIDEO_ENC_PARAMS of type
// AV_VIDEO_ENC_PARAMS_HEVC (one block per coding unit with its QP_Y) and AV_FRAME_DATA_MOTION_VECTORS (one vector per
// list and per unit of the motion vector field). Only motion vectors and QP are mapped: no partitions, intra modes,
// transform tree or references beyond the pseudo slots L0 / L1. Plain structs out (plugin.cpp builds the FlatBuffers).
#ifndef OVT_PLUGIN_FFMPEG_MAP_HEVC_SIDEDATA_HPP
#define OVT_PLUGIN_FFMPEG_MAP_HEVC_SIDEDATA_HPP

#include <vector>

#include "map_h264_sidedata.hpp"

struct AVFrame;

namespace ovt::ffmpeg {

/// Maps the side data attached to @p frame (coded size, apply_cropping = 0) into the shared SideData layout: `mbs`
/// holds the coding units in the order of the side data (raster order of their top-left corners), with QP_Y as
/// exported (no QpBdOffsetY: it can be negative above 8 bits) and pred Inter when a vector lands in the CU, else
/// Intra. `valid` is false without AV_VIDEO_ENC_PARAMS_HEVC side data (an unpatched libavcodec). Vectors of one list,
/// one CU and one value are merged into rectangles. Diagnostics (MDL-GEN-0003 clamped vectors, a code-less warning
/// for side data outside the picture) are appended to @p diags.
SideData map_hevc_sidedata(const AVFrame* frame, std::vector<Diag>* diags);

}  // namespace ovt::ffmpeg

#endif  // OVT_PLUGIN_FFMPEG_MAP_HEVC_SIDEDATA_HPP
