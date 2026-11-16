// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PNG export (chapter 10 §10.8, WI-1.412): colour targets, colour-space chunks, sample packing and the lodepng encode.
// See png_export.h.

#include "export/png_export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <lodepng.h>

namespace ovt::render::detail {
namespace {

// H.273 code points of the §10.8 targets.
constexpr uint8_t kPrimBt709 = 1;
constexpr uint8_t kPrimBt2020 = 9;
constexpr uint8_t kTrcSrgb = 13;
constexpr uint8_t kTrcPq = 16;
constexpr uint8_t kTrcHlg = 18;

uint32_t scaled(float v, double unit, double max) {
  if (!(v > 0.0F)) {  // also NaN
    return 0;
  }
  return static_cast<uint32_t>(std::min(std::llround(static_cast<double>(v) * unit), static_cast<long long>(max)));
}

// lodepng's state, released on every path.
struct LodeState {
  LodePNGState s{};
  LodeState() { lodepng_state_init(&s); }
  ~LodeState() { lodepng_state_cleanup(&s); }
  LodeState(const LodeState&) = delete;
  LodeState& operator=(const LodeState&) = delete;
  LodeState(LodeState&&) = delete;
  LodeState& operator=(LodeState&&) = delete;
};

}  // namespace

ExportTarget export_target(ExportKind kind, const struct pl_color_space& source) {
  ExportTarget t;
  t.color = pl_color_space_srgb;
  t.color.hdr.max_luma = PL_COLOR_SDR_WHITE;
  t.cicp = PngCicp{kPrimBt709, kTrcSrgb, 0, 1};
  if (kind != ExportKind::HdrPng16) {
    return t;  // SdrPng8 (and the 8-bit default for any other kind)
  }
  t.bit_depth = 16;
  if (source.transfer == PL_COLOR_TRC_PQ || source.transfer == PL_COLOR_TRC_HLG) {
    const bool pq = source.transfer == PL_COLOR_TRC_PQ;
    t.color = source;  // the source's HDR metadata: libplacebo then maps nothing but primaries
    t.color.primaries = PL_COLOR_PRIM_BT_2020;
    t.hdr_pass = true;
    t.cicp = PngCicp{kPrimBt2020, pq ? kTrcPq : kTrcHlg, 0, 1};
  }
  return t;
}

// SPEC-ASSUMPTION(#637): §10.8 names only cICP; mDCV/cLLi (PNG Third Edition) carry the source's static metadata, set
// through lodepng's native fields rather than lodepng_chunk_create.
PngHdrChunks hdr_chunks(const struct pl_hdr_metadata& hdr) {
  PngHdrChunks c;
  const struct pl_raw_primaries& p = hdr.prim;
  const bool prim = p.red.x > 0.0F && p.red.y > 0.0F && p.green.x > 0.0F && p.green.y > 0.0F && p.blue.x > 0.0F &&
                    p.blue.y > 0.0F && p.white.x > 0.0F && p.white.y > 0.0F;
  if (prim && hdr.max_luma > 0.0F) {
    constexpr double kXy = 50000.0;   // mDCV chromaticity unit 0.00002
    constexpr double kLum = 10000.0;  // mDCV / cLLi luminance unit 0.0001 cd/m²
    constexpr double kU16 = 65535.0;
    constexpr double kU32 = 4294967295.0;
    c.has_mdcv = true;
    c.red_x = scaled(p.red.x, kXy, kU16);
    c.red_y = scaled(p.red.y, kXy, kU16);
    c.green_x = scaled(p.green.x, kXy, kU16);
    c.green_y = scaled(p.green.y, kXy, kU16);
    c.blue_x = scaled(p.blue.x, kXy, kU16);
    c.blue_y = scaled(p.blue.y, kXy, kU16);
    c.white_x = scaled(p.white.x, kXy, kU16);
    c.white_y = scaled(p.white.y, kXy, kU16);
    c.max_luminance = scaled(hdr.max_luma, kLum, kU32);
    c.min_luminance = scaled(hdr.min_luma, kLum, kU32);
  }
  if (hdr.max_cll > 0.0F) {
    constexpr double kLum = 10000.0;
    constexpr double kU32 = 4294967295.0;
    c.has_clli = true;
    c.max_cll = scaled(hdr.max_cll, kLum, kU32);
    c.max_fall = scaled(hdr.max_fall, kLum, kU32);
  }
  return c;
}

bool pack_samples(const std::vector<uint8_t>& texels, size_t texel_bytes, int width, int height, int channels,
                  PngImage& image) {
  if (width <= 0 || height <= 0 || (channels != 3 && channels != 4) || (texel_bytes != 4 && texel_bytes != 8)) {
    return false;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (texels.size() != n * texel_bytes) {
    return false;
  }
  const auto ch = static_cast<size_t>(channels);
  image.width = width;
  image.height = height;
  image.channels = channels;
  image.bit_depth = texel_bytes == 8 ? 16 : 8;
  image.samples.assign(n * ch * (texel_bytes / 4), 0);
  std::vector<uint8_t>& out = image.samples;
  for (size_t i = 0; i < n; ++i) {
    for (size_t c = 0; c < ch; ++c) {
      const size_t o = (i * ch) + c;
      if (texel_bytes == 4) {
        out[o] = texels[(i * 4) + c];
      } else {
        // A host-order uint16 texel component, written big-endian (PNG sample order).
        uint16_t v = 0;
        std::memcpy(&v, &texels[(i * 8) + (c * 2)], sizeof v);
        out[o * 2] = static_cast<uint8_t>(v >> 8U);
        out[(o * 2) + 1] = static_cast<uint8_t>(v & 0xFFU);
      }
    }
  }
  return true;
}

bool has_transparency(const std::vector<uint8_t>& rgba8) {
  for (size_t i = 3; i < rgba8.size(); i += 4) {
    if (rgba8[i] != 0xFFU) {
      return true;
    }
  }
  return false;
}

bool encode_png(const PngImage& image, std::vector<uint8_t>& png, std::string& error) {
  png.clear();
  const size_t expected = static_cast<size_t>(std::max(image.width, 0)) *
                          static_cast<size_t>(std::max(image.height, 0)) * static_cast<size_t>(image.channels) *
                          static_cast<size_t>(image.bit_depth / 8);
  if (image.width <= 0 || image.height <= 0 || (image.channels != 3 && image.channels != 4) ||
      (image.bit_depth != 8 && image.bit_depth != 16) || image.samples.size() != expected) {
    error = "invalid image";
    return false;
  }
  LodeState st;
  const LodePNGColorType type = image.channels == 4 ? LCT_RGBA : LCT_RGB;
  const auto depth = static_cast<unsigned>(image.bit_depth);
  st.s.info_raw.colortype = type;
  st.s.info_raw.bitdepth = depth;
  st.s.info_png.color.colortype = type;
  st.s.info_png.color.bitdepth = depth;
  st.s.encoder.auto_convert = 0;  // keep colour type 2/6 and the depth (§10.8), never a palette or grey
  st.s.encoder.add_id = 0;        // no "LodePNG" tEXt chunk
  LodePNGInfo& info = st.s.info_png;
  if (image.has_cicp) {
    info.cicp_defined = 1;
    info.cicp_color_primaries = image.cicp.primaries;
    info.cicp_transfer_function = image.cicp.transfer;
    info.cicp_matrix_coefficients = image.cicp.matrix;
    info.cicp_video_full_range_flag = image.cicp.full_range;
  }
  const PngHdrChunks& h = image.hdr;
  if (h.has_mdcv) {
    info.mdcv_defined = 1;
    info.mdcv_red_x = h.red_x;
    info.mdcv_red_y = h.red_y;
    info.mdcv_green_x = h.green_x;
    info.mdcv_green_y = h.green_y;
    info.mdcv_blue_x = h.blue_x;
    info.mdcv_blue_y = h.blue_y;
    info.mdcv_white_x = h.white_x;
    info.mdcv_white_y = h.white_y;
    info.mdcv_max_luminance = h.max_luminance;
    info.mdcv_min_luminance = h.min_luminance;
  }
  if (h.has_clli) {
    info.clli_defined = 1;
    info.clli_max_cll = h.max_cll;
    info.clli_max_fall = h.max_fall;
  }
  unsigned char* out = nullptr;
  size_t out_size = 0;
  const unsigned err = lodepng_encode(&out, &out_size, image.samples.data(), static_cast<unsigned>(image.width),
                                      static_cast<unsigned>(image.height), &st.s);
  const std::unique_ptr<unsigned char, void (*)(void*)> owned(out, &std::free);  // lodepng allocates with malloc
  if (err != 0) {
    error = lodepng_error_text(err);
    return false;
  }
  png.assign(out, out + out_size);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return true;
}

bool write_file(const char* path, const std::vector<uint8_t>& bytes) {
  if (path == nullptr || *path == '\0') {
    return false;
  }
  const std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path, "wb"), &std::fclose);
  if (f == nullptr) {
    return false;
  }
  return std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size() && std::fflush(f.get()) == 0;
}

std::string default_export_name(const std::string& stream, int64_t decode_index, int64_t display_index,
                                ExportKind kind) {
  const char* name = "ViewPng8";
  switch (kind) {
    case ExportKind::HdrPng16:
      name = "HdrPng16";
      break;
    case ExportKind::SdrPng8:
      name = "SdrPng8";
      break;
    case ExportKind::ViewPng8:
      break;
    case ExportKind::ScopeWaveformPng:
      name = "ScopeWaveformPng";
      break;
    case ExportKind::ScopeVectorscopePng:
      name = "ScopeVectorscopePng";
      break;
    case ExportKind::ScopeCiePng:
      name = "ScopeCiePng";
      break;
  }
  return stream + "_" + std::to_string(decode_index) + "_" + std::to_string(display_index) + "_" + name + ".png";
}

}  // namespace ovt::render::detail
