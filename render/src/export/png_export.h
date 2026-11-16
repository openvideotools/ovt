// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The CPU half of VideoSurface::export_png() for HdrPng16, SdrPng8 and ViewPng8 (chapter 10 §10.8, WI-1.412): which
// colour space each kind renders to, the PNG colour-space chunks (cICP, and mDCV/cLLi for HDR), the texel repacking
// of the downloaded texture and the PNG encode through the vendored lodepng (third_party/lodepng). The GPU half (the
// offscreen render and the download) is in render/src/surface/video_surface_impl.cpp.
//
// Internal to libovt-render; the functions are exported for the render tests only.

#ifndef OVT_RENDER_SRC_EXPORT_PNG_EXPORT_H
#define OVT_RENDER_SRC_EXPORT_PNG_EXPORT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>

#include "ovt/render/export.h"
#include "ovt/render/render_api.h"

namespace ovt::render::detail {

// cICP chunk payload (PNG Third Edition §11.3.2.6): H.273 code points.
struct PngCicp {
  uint8_t primaries = 0;
  uint8_t transfer = 0;
  uint8_t matrix = 0;
  uint8_t full_range = 1;
};

// mDCV and cLLi payloads (PNG Third Edition §11.3.2.7, §11.3.2.8) in the chunks' own units: chromaticities in
// 0.00002 steps, luminances in 0.0001 cd/m².
struct PngHdrChunks {
  bool has_mdcv = false;
  uint32_t red_x = 0, red_y = 0, green_x = 0, green_y = 0, blue_x = 0, blue_y = 0, white_x = 0, white_y = 0;
  uint32_t max_luminance = 0, min_luminance = 0;
  bool has_clli = false;
  uint32_t max_cll = 0, max_fall = 0;
};

// What one picture export renders to: the libplacebo target colour space, the PNG sample depth and the cICP chunk.
struct ExportTarget {
  struct pl_color_space color = {};
  int bit_depth = 8;      // 8 (texture "rgba8") or 16 (texture "rgba16")
  bool hdr_pass = false;  // HdrPng16 of a PQ or HLG source: the source's own transfer and HDR metadata, no tone mapping
  PngCicp cicp;
};

// §10.8 rules for HdrPng16 and SdrPng8, given the colour space of the picture in slot A:
//   HdrPng16, PQ source   BT.2020 + PQ, 16 bit, cICP 9,16,0,1
//   HdrPng16, HLG source  BT.2020 + HLG (data stays HLG-encoded), 16 bit, cICP 9,18,0,1
//   HdrPng16, SDR source  BT.709 + sRGB, 16 bit, cICP 1,13,0,1
//   SdrPng8               BT.709 + sRGB, 8 bit, cICP 1,13,0,1 (tone-mapped with the ViewState)
// ViewPng8 renders in the surface's Srgb8 encoding instead (no ExportTarget).
OVT_RENDER_API ExportTarget export_target(ExportKind kind, const struct pl_color_space& source);

// The mDCV/cLLi chunks of an HdrPng16 export of an HDR source: mDCV when the source carries mastering primaries and a
// mastering peak, cLLi when it carries MaxCLL. Values out of the chunks' ranges are clamped.
OVT_RENDER_API PngHdrChunks hdr_chunks(const struct pl_hdr_metadata& hdr);

// An image ready for the PNG encoder: rows top first, tightly packed, 16-bit samples big-endian (PNG order).
struct PngImage {
  int width = 0;
  int height = 0;
  int channels = 3;   // 3 (colour type 2, RGB) or 4 (colour type 6, RGBA)
  int bit_depth = 8;  // 8 or 16
  std::vector<uint8_t> samples;
  bool has_cicp = false;
  PngCicp cicp;
  PngHdrChunks hdr;
};

// Repacks downloaded texels into PngImage samples. `texels` holds width x height RGBA texels of `texel_bytes` (4: 8-bit
// UNORM, 8: 16-bit UNORM in host byte order); `channels` 3 drops alpha. Returns false on a size mismatch.
OVT_RENDER_API bool pack_samples(const std::vector<uint8_t>& texels, size_t texel_bytes, int width, int height,
                                 int channels, PngImage& image);

// True when any 8-bit RGBA texel has alpha below 255 (ViewPng8 then keeps the alpha channel).
OVT_RENDER_API bool has_transparency(const std::vector<uint8_t>& rgba8);

// Encodes `image` as a PNG: colour type and depth exactly as given (no automatic conversion), the colour-space chunks
// before IDAT, no text chunks, lodepng's default deflate settings. The output depends only on the input, so the same
// picture exports to the same bytes (§10.9.2 item 5). Returns false with lodepng's message in `error`.
OVT_RENDER_API bool encode_png(const PngImage& image, std::vector<uint8_t>& png, std::string& error);

// Writes `bytes` to `path` (replacing it). Returns false when the file cannot be written.
OVT_RENDER_API bool write_file(const char* path, const std::vector<uint8_t>& bytes);

// The §10.8 default file name `<stream>_<decode_index>_<display_index>_<kind>.png`, with <kind> the ExportKind
// enumerator's name (HdrPng16, SdrPng8, ViewPng8, ScopeWaveformPng, ...).
OVT_RENDER_API std::string default_export_name(const std::string& stream, int64_t decode_index, int64_t display_index,
                                               ExportKind kind);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_EXPORT_PNG_EXPORT_H
