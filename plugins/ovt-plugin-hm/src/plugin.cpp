// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// libovt_plugin_hm: the ABI glue of the HEVC oracle plugin (chapter 0 §0.6.2, chapter 07 §7.6.4, WI-4.105), which
// wraps HM, the JCT-VC reference decoder. It exports only ovt_analysis_decoder_v1(); every entry point is noexcept
// and turns an exception into an error status. Part a of WI-4.105 produces frame-level FrameAnalysis buffers
// (FrameInfo, the 4x4 cell grid size, diagnostics) and the Reconstruction planes; part b adds the TComDataCU mapping.
//
// HM is BSD-3-Clause (third_party/VERSIONS.md); its COPYING grants no patent rights, and HEVC is patent-encumbered
// (chapter 15), so the plugin is optional (OVT_BUILD_PLUGIN_HM) and never in the default downloads.

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "decoder.hpp"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"

namespace {

constexpr uint8_t kCodecH265 = 2;    // ovt.fb.Codec.H265
constexpr uint32_t kStageRecon = 0;  // ovt.fb.PlaneStage.Reconstruction
constexpr std::array<const char*, 3> kPlaneNames = {"y", "u", "v"};

}  // namespace

struct ovt_AnalysisDecoder {
  // A FrameAnalysis handed out by pull() and not yet released, with the planes its PlaneRefs name.
  struct Pulled {
    std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb;
    ovt::hm::Frame frame;
    std::map<std::string, const ovt::hm::Plane*> blobs;  // blob_id -> plane of `frame`
  };
  std::unique_ptr<ovt::hm::Decoder> decoder;
  std::map<const uint8_t*, std::unique_ptr<Pulled>> pulled;
};

namespace {

const ovt_AnalysisCaps kCaps = {
    sizeof(ovt_AnalysisCaps),
    kCodecH265,
    1U << kStageRecon,     // plane_stages: Reconstruction (WI-4.105 part b adds Coefficients)
    ovt::hm::kGridFields,  // grid_fields: all but partition_id, bits_x8 and deblock_bs (analysis.hpp)
    0,                     // has_symbol_accounting
    1,                     // bit_exact: the reference decoder
    0,                     // supports_threads: HM decodes on the calling thread
    "hm",
    "0.1.0+HM-18.0",
    "BSD-3-Clause",
    "HEVC oracle: HM-18.0 (JCT-VC reference decoder), base layer of Main, Main 10, RExt and SCC profiles as far as "
    "HM-18.0 decodes them; slow (verify runs, RExt). Pictures come out in decode order.",
};

// The plugin-local blob id "<decode_index>/recon/<plane>" (chapter 06 §6.6.2); the host prefixes the source hash.
std::string blob_id(int32_t decode_index, int plane) {
  return std::to_string(decode_index) + "/recon/" + kPlaneNames.at(static_cast<std::size_t>(plane));
}

std::unique_ptr<ovt_AnalysisDecoder::Pulled> build(ovt::hm::Frame frame, uint32_t plane_stage_mask) {
  auto out = std::make_unique<ovt_AnalysisDecoder::Pulled>();
  out->frame = std::move(frame);
  const ovt::hm::Frame& f = out->frame;
  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(1024);

  std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> diags;
  diags.reserve(f.diagnostics.size());
  for (const ovt::hm::Diag& d : f.diagnostics) {
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
  if (f.picture && (plane_stage_mask & (1U << kStageRecon)) != 0) {
    for (int c = 0; c < 3; ++c) {
      const ovt::hm::Plane& p = f.planes[static_cast<std::size_t>(c)];
      if (p.data.empty()) {
        continue;
      }
      std::string id = blob_id(f.decode_index, c);
      const auto id_off = fbb->CreateString(id);
      ovt::fb::PlaneRefBuilder pb(*fbb);
      pb.add_stage(ovt::fb::PlaneStage::Reconstruction);
      pb.add_plane(static_cast<uint8_t>(c));
      pb.add_width(p.width);
      pb.add_height(p.height);
      pb.add_stride_bytes(p.width * p.sample_bytes);
      pb.add_bit_depth(p.bit_depth);
      pb.add_sample_bytes(p.sample_bytes);
      pb.add_is_signed(false);
      pb.add_blob_id(id_off);
      planes.push_back(pb.Finish());
      out->blobs.emplace(std::move(id), &p);
    }
  }
  const auto planes_v = fbb->CreateVector(planes);

  ovt::hm::MappedAnalysis mapped;
  if (f.picture) {
    mapped = ovt::hm::build_analysis(*fbb, f.analysis, f.coded_w, f.coded_h);
  }

  const ovt::fb::FrameId id(f.decode_index, -1);  // the host assigns display_index from its index
  const ovt::fb::Rect coded(0, 0, f.coded_w, f.coded_h);
  const ovt::fb::Rect crop(f.crop_x, f.crop_y, f.crop_w, f.crop_h);
  ovt::fb::FrameInfoBuilder info(*fbb);
  info.add_id(&id);
  info.add_pts(f.pts);
  if (f.picture) {
    info.add_type(static_cast<ovt::fb::FrameType>(f.frame_type));
    info.add_shown(f.shown);  // PicOutputFlag: HM decodes pictures with PicOutputFlag = 0 too
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
    fa.add_grid(mapped.grid);
    fa.add_blocks(mapped.blocks);
    fa.add_mvs(mapped.mvs);
    fa.add_refs(mapped.refs);
  }
  fa.add_planes(planes_v);
  fa.add_diagnostics(diags_v);
  ovt::fb::FinishFrameAnalysisBuffer(*fbb, fa.Finish());
  out->fbb = std::move(fbb);
  return out;
}

const ovt_AnalysisCaps* hm_caps() noexcept {
  return &kCaps;
}

ovt_status hm_create(const uint8_t* codec_private, size_t len, int /*threads*/, ovt_AnalysisDecoder** out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    auto dec = std::make_unique<ovt_AnalysisDecoder>();
    const ovt_status st = ovt::hm::Decoder::create(codec_private, len, &dec->decoder);
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

void hm_destroy(ovt_AnalysisDecoder* dec) noexcept {
  delete dec;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status hm_push(ovt_AnalysisDecoder* dec, const uint8_t* unit, size_t len, int32_t decode_index_hint,
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

ovt_status hm_pull(ovt_AnalysisDecoder* dec, uint32_t plane_stage_mask, ovt_Buffer* out_fb) noexcept {
  if (dec == nullptr || out_fb == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out_fb = ovt_Buffer{nullptr, 0};
  try {
    ovt::hm::Frame frame;
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

ovt_status hm_plane(ovt_AnalysisDecoder* dec, const char* blob_id, const uint8_t** data, size_t* size) noexcept {
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

void hm_release(ovt_AnalysisDecoder* dec, ovt_Buffer fb) noexcept {
  if (dec != nullptr) {
    dec->pulled.erase(fb.data);
  }
}

ovt_status hm_flush(ovt_AnalysisDecoder* dec) noexcept {
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
    hm_caps,
    hm_create,
    hm_destroy,
    hm_push,
    hm_pull,
    hm_plane,
    hm_release,
    hm_flush,
};

}  // namespace

extern "C" OVT_PLUGIN_EXPORT const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void) {
  return &kApi;
}
