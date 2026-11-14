// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// libovt_plugin_de265: the ABI glue of the HEVC analysis plugin (chapter 0 §0.6.2, chapter 07 §7.6.2, WI-2.163).
// It exports only ovt_analysis_decoder_v1(); every entry point is noexcept and turns an exception into an error
// status. This skeleton produces frame-level FrameAnalysis buffers (FrameInfo, the empty 4x4 cell grid size,
// diagnostics) and the planes (planes.hpp: Reconstruction, and the patch-0003 stages of WI-2.166); the block data
// of the mapping table arrives with WI-2.164.

#include <flatbuffers/flatbuffers.h>
#include <libde265/de265-version.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "decoder.hpp"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"
#include "planes.hpp"

namespace {

constexpr uint8_t kCodecH265 = 2;  // ovt.fb.Codec.H265

}  // namespace

struct ovt_AnalysisDecoder {
  // A FrameAnalysis handed out by pull() and not yet released, with the planes its PlaneRefs name.
  struct Pulled {
    std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb;
    ovt::de265::Frame frame;
    std::map<std::string, const ovt::de265::Plane*> blobs;  // blob_id -> plane of `frame`
  };
  std::unique_ptr<ovt::de265::Decoder> decoder;
  std::map<const uint8_t*, std::unique_ptr<Pulled>> pulled;
};

namespace {

const ovt_AnalysisCaps kCaps = {
    sizeof(ovt_AnalysisCaps),
    kCodecH265,
    ovt::de265::kSupportedStageMask,  // plane_stages: Reconstruction, + the patch-0003 stages (WI-2.166)
    0U,                               // grid_fields: none yet; WI-2.164 fills the CellGrid vectors and announces them
    0,                                // has_symbol_accounting
    1,                                // bit_exact: conformance-exact for Main / Main 10
    1,                                // supports_threads: libde265 worker threads
    "de265",
    "0.1.0+libde265." LIBDE265_VERSION,
    "LGPL-3.0-or-later",
    "HEVC Main / Main 10 / Main Still Picture via libde265 " LIBDE265_VERSION
    " (OVT patches 0001, 0003); RExt, SCC and multi-layer streams: use ovt-plugin-hm",
};

std::unique_ptr<ovt_AnalysisDecoder::Pulled> build(ovt::de265::Frame frame, uint32_t plane_stage_mask) {
  auto out = std::make_unique<ovt_AnalysisDecoder::Pulled>();
  out->frame = std::move(frame);
  ovt::de265::keep_stages(&out->frame, plane_stage_mask);
  const ovt::de265::Frame& f = out->frame;
  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(1024);

  std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> diags;
  diags.reserve(f.diagnostics.size());
  for (const ovt::de265::Diag& d : f.diagnostics) {
    const auto code = fbb->CreateString(d.code);
    const auto message = fbb->CreateString(d.message);
    ovt::fb::DiagnosticBuilder db(*fbb);
    db.add_severity(static_cast<ovt::fb::Severity>(d.severity));
    db.add_code(code);
    db.add_message(message);
    diags.push_back(db.Finish());
  }
  const auto diags_v = fbb->CreateVector(diags);

  std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>> planes;
  ovt::de265::add_plane_refs(*fbb, f, plane_stage_mask, &planes, &out->blobs);
  const auto planes_v = fbb->CreateVector(planes);

  flatbuffers::Offset<ovt::fb::CellGrid> grid_off;
  if (f.picture) {
    ovt::fb::CellGridBuilder grid(*fbb);
    grid.add_cells_w(static_cast<uint32_t>((f.coded_w + 3) / 4));
    grid.add_cells_h(static_cast<uint32_t>((f.coded_h + 3) / 4));
    grid_off = grid.Finish();
  }

  const ovt::fb::FrameId id(f.decode_index, -1);  // the host assigns display_index from its index
  const ovt::fb::Rect coded(0, 0, f.coded_w, f.coded_h);
  const ovt::fb::Rect crop(f.crop_x, f.crop_y, f.crop_w, f.crop_h);
  ovt::fb::FrameInfoBuilder info(*fbb);
  info.add_id(&id);
  info.add_pts(f.pts);
  if (f.picture) {
    info.add_type(static_cast<ovt::fb::FrameType>(f.frame_type));
    info.add_shown(true);  // libde265 outputs only pictures with PicOutputFlag = 1
    info.add_temporal_id(f.temporal_id);
    info.add_layer_id(f.layer_id);
    info.add_is_rap(f.is_rap);
    info.add_poc(f.poc);
    info.add_coded_size(&coded);
    info.add_crop(&crop);
    info.add_chroma_format(f.chroma_format);
    info.add_bit_depth_luma(f.bit_depth_luma);
    info.add_bit_depth_chroma(f.bit_depth_chroma);
    info.add_qp_base(f.qp_base);
    info.add_slice_count(f.slice_count);
  }
  const auto info_off = info.Finish();

  const auto plugin_id = fbb->CreateString(kCaps.id);
  const auto plugin_version = fbb->CreateString(kCaps.version);
  ovt::fb::FrameAnalysisBuilder fa(*fbb);
  fa.add_schema_version(1);
  fa.add_plugin_id(plugin_id);
  fa.add_plugin_version(plugin_version);
  fa.add_frame(info_off);
  if (f.picture) {
    fa.add_grid(grid_off);
  }
  fa.add_planes(planes_v);
  fa.add_diagnostics(diags_v);
  ovt::fb::FinishFrameAnalysisBuffer(*fbb, fa.Finish());
  out->fbb = std::move(fbb);
  return out;
}

const ovt_AnalysisCaps* de265_caps() noexcept {
  return &kCaps;
}

ovt_status de265_create(const uint8_t* codec_private, size_t len, int threads, ovt_AnalysisDecoder** out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    auto dec = std::make_unique<ovt_AnalysisDecoder>();
    const ovt_status st = ovt::de265::Decoder::create(codec_private, len, threads, &dec->decoder);
    if (st != OVT_OK) {
      return st;
    }
    *out = dec.release();
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

void de265_destroy(ovt_AnalysisDecoder* dec) noexcept {
  delete dec;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status de265_push(ovt_AnalysisDecoder* dec, const uint8_t* unit, size_t len, int32_t decode_index_hint,
                      int64_t pts) noexcept {
  if (dec == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    return dec->decoder->push(unit, len, decode_index_hint, pts);
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

ovt_status de265_pull(ovt_AnalysisDecoder* dec, uint32_t plane_stage_mask, ovt_Buffer* out_fb) noexcept {
  if (dec == nullptr || out_fb == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out_fb = ovt_Buffer{nullptr, 0};
  try {
    dec->decoder->request_stages(plane_stage_mask);
    ovt::de265::Frame frame;
    if (!dec->decoder->pop(&frame)) {
      return OVT_OK;
    }
    auto pulled = build(std::move(frame), plane_stage_mask);
    *out_fb = ovt_Buffer{pulled->fbb->GetBufferPointer(), pulled->fbb->GetSize()};
    dec->pulled.emplace(out_fb->data, std::move(pulled));
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    *out_fb = ovt_Buffer{nullptr, 0};
    return OVT_E_NOMEM;
  } catch (...) {
    *out_fb = ovt_Buffer{nullptr, 0};
    return OVT_E_PLUGIN;
  }
}

ovt_status de265_plane(ovt_AnalysisDecoder* dec, const char* blob_id, const uint8_t** data, size_t* size) noexcept {
  if (dec == nullptr || blob_id == nullptr || data == nullptr || size == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *data = nullptr;
  *size = 0;
  try {
    for (const auto& [ptr, pulled] : dec->pulled) {
      if (const auto it = pulled->blobs.find(blob_id); it != pulled->blobs.end()) {
        *data = it->second->data.data();
        *size = it->second->data.size();
        return OVT_OK;
      }
    }
    return OVT_E_INVALID_ARG;  // unknown, or its FrameAnalysis was already released
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

void de265_release(ovt_AnalysisDecoder* dec, ovt_Buffer fb) noexcept {
  if (dec != nullptr) {
    dec->pulled.erase(fb.data);
  }
}

ovt_status de265_flush(ovt_AnalysisDecoder* dec) noexcept {
  if (dec == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    return dec->decoder->flush();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

const ovt_AnalysisDecoderApi kApi = {
    sizeof(ovt_AnalysisDecoderApi),
    OVT_ANALYSIS_ABI_VERSION,
    de265_caps,
    de265_create,
    de265_destroy,
    de265_push,
    de265_pull,
    de265_plane,
    de265_release,
    de265_flush,
};

}  // namespace

extern "C" OVT_PLUGIN_EXPORT const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void) {
  return &kApi;
}
