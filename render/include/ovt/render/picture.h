// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Picture and PlaneData: the decoded picture handed to a VideoSurface (chapter 10 §10.2.1, verbatim).

#ifndef OVT_RENDER_PICTURE_H
#define OVT_RENDER_PICTURE_H

#include <cstddef>
#include <cstdint>

#include "ovt/color.h"  // ovt_ColorDescription
// SPEC-ASSUMPTION(#209): §10.2.1 writes <ovt/model_generated.h>; the generated header is ovt_model_generated.h.
#include "ovt_model_generated.h"  // ovt::fb::FrameId, ovt::fb::Rect

namespace ovt::render {

struct PlaneData {
  const void* data = nullptr;  // first sample of row 0; MUST stay valid until set_frame() returns
  uint32_t width = 0;          // samples in this plane (chroma planes are subsampled)
  uint32_t height = 0;
  uint32_t stride_bytes = 0;  // >= width * sample_bytes; may include padding
  uint8_t sample_bytes = 1;   // 1 for bit_depth <= 8, 2 for 9..16 (little-endian, LSB-aligned)
};

// NOLINTBEGIN(modernize-avoid-c-arrays,readability-isolate-declaration): verbatim from §10.2.1
struct Picture {
  ovt::fb::FrameId id;
  uint32_t coded_width = 0, coded_height = 0;  // luma samples before cropping
  ovt::fb::Rect crop;                          // conformance window / render size (§0.1.9)
  uint8_t num_planes = 3;                      // 1 for 4:0:0, otherwise 3
  PlaneData planes[3];                         // 0=Y 1=U(Cb) 2=V(Cr)
  ovt_ColorDescription color{};  // chroma_format, bit_depth, primaries, transfer, matrix, range, siting, static HDR
  bool interlaced = false, top_field_first = true;  // from FrameInfo (chapter 06); drives ViewState.field_mode
  // Dynamic metadata (nullable). Raw payloads as carried in the stream; the renderer parses them.
  // ST 2094-40 SEI payload (after itu_t_t35 header) / AV1 METADATA_TYPE_ITUT_T35.
  const uint8_t* hdr10plus_payload = nullptr;
  size_t hdr10plus_len = 0;
  // NOT raw RPU bytes despite the name (kept for ABI stability): the parsed Dolby Vision metadata as the libavutil
  // AVDOVIMetadata side-data blob (AV_FRAME_DATA_DOVI_METADATA, §10.3.7); nullptr when the decoder exposes none.
  const uint8_t* dovi_rpu = nullptr;
  size_t dovi_rpu_len = 0;
  // Monotonic generation counter. If two consecutive set_frame() calls carry the same generation for the same slot,
  // the surface MUST skip re-upload.
  uint64_t generation = 0;
};
// NOLINTEND(modernize-avoid-c-arrays,readability-isolate-declaration)

}  // namespace ovt::render

#endif  // OVT_RENDER_PICTURE_H
