// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// libovt_plugin_aom: the ABI glue of the AV1 analysis plugin (chapter 0 §0.6.2, chapter 07 §7.6.1, WI-2.155).
// It exports ovt_analysis_decoder_v1() and nothing else; every entry point is noexcept and turns an exception into
// an error status. It produces FrameAnalysis buffers with FrameInfo, the cell grid, block tree, motion vectors,
// reference slots and tiles mapped from libaom's insp_mi_data (WI-2.156), diagnostics, and the Reconstruction
// planes of shown frames. WI-2.157 adds the accounting.

#include <aom/aom_codec.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "decoder.hpp"
#include "fb_builder.hpp"
#include "map_mi.hpp"
#include "ovt/abi/analysis_decoder_v1.h"

namespace {

constexpr uint8_t kCodecAv1 = 3;     // ovt.fb.Codec.AV1
constexpr uint32_t kStageRecon = 0;  // ovt.fb.PlaneStage.Reconstruction

}  // namespace

struct ovt_AnalysisDecoder {
  // A FrameAnalysis handed out by pull() and not yet released, with the planes its PlaneRefs name.
  struct Pulled {
    std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb;
    ovt::aom::Frame frame;
    std::map<std::string, const ovt::aom::Plane*> blobs;  // blob_id -> plane of `frame`
  };
  std::unique_ptr<ovt::aom::Decoder> decoder;
  std::map<const uint8_t*, std::unique_ptr<Pulled>> pulled;
};

namespace {

// Strings and caps are built once from the linked libaom (static locals: thread-safe initialisation).
struct Info {
  std::string version = std::string("0.1.0+libaom.") + aom_codec_version_str();
  std::string notes =
      std::string("AV1 analysis via libaom ") + aom_codec_version_str() +
      " inspection (blocks, motion vectors, reference slots, tiles and reconstruction; no symbol accounting yet)";
  ovt_AnalysisCaps caps{};
  Info() {
    caps.struct_size = sizeof(ovt_AnalysisCaps);
    caps.codec = kCodecAv1;
    caps.plane_stages = 1U << kStageRecon;     // stock libaom: Reconstruction only (§7.6.1 Caps row)
    caps.grid_fields = ovt::aom::kGridFields;  // §7.6.1 Caps row with stock libaom, before accounting (WI-2.157)
    caps.has_symbol_accounting = 0;            // WI-2.157
    caps.bit_exact = 1;
    caps.supports_threads = 1;  // safe; libaom runs on one thread while accounting is on (decoder.cpp)
    caps.id = "aom";
    caps.version = version.c_str();
    caps.license = "BSD-2-Clause AND ISC AND LicenseRef-AOM-Patent-License-1.0";  // plugin + static libaom
    caps.notes = notes.c_str();
  }
};

const Info& info() {
  static const Info i;
  return i;
}

const ovt_AnalysisCaps* aom_caps() noexcept {
  try {
    return &info().caps;
  } catch (...) {
    return nullptr;  // out of memory while building the strings; the registry rejects the plugin
  }
}

// Runs @p body, turning an exception into OVT_E_NOMEM / OVT_E_PLUGIN: no exception crosses the ABI.
template <typename F>
ovt_status guarded(F&& body) noexcept {
  try {
    return body();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

// codec_private (av1C) is not needed: the host pushes the sequence header OBU active at the random access point
// (§7.5.3 A3), and libaom reads configOBUs from the stream.
ovt_status aom_create(const uint8_t* /*codec_private*/, size_t /*len*/, int threads,
                      ovt_AnalysisDecoder** out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  return guarded([&] {
    auto dec = std::make_unique<ovt_AnalysisDecoder>();
    std::string error;
    // SPEC-ASSUMPTION(#335): the ABI has no message channel for create(); the message stays in
    // ovt::aom::Decoder::create's error text (tests read it there).
    const ovt_status st = ovt::aom::Decoder::create(threads, &dec->decoder, &error);
    if (st == OVT_OK) {
      *out = dec.release();
    }
    return st;
  });
}

void aom_destroy(ovt_AnalysisDecoder* dec) noexcept {
  delete dec;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status aom_push(ovt_AnalysisDecoder* dec, const uint8_t* unit, size_t len, int32_t decode_index_hint,
                    int64_t pts) noexcept {
  if (dec == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return dec->decoder->push(unit, len, decode_index_hint, pts); });
}

ovt_status aom_pull(ovt_AnalysisDecoder* dec, uint32_t plane_stage_mask, ovt_Buffer* out_fb) noexcept {
  if (dec == nullptr || out_fb == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out_fb = ovt_Buffer{nullptr, 0};
  return guarded([&] {
    auto pulled = std::make_unique<ovt_AnalysisDecoder::Pulled>();
    if (!dec->decoder->pop(&pulled->frame)) {
      return OVT_OK;
    }
    if ((plane_stage_mask & (1U << kStageRecon)) == 0) {
      pulled->frame.planes = {};  // not requested: drop the copy now
    }
    pulled->fbb = ovt::aom::build_frame_analysis(pulled->frame, plane_stage_mask, info().caps.id, info().caps.version,
                                                 &pulled->blobs);
    const ovt_Buffer fb{pulled->fbb->GetBufferPointer(), pulled->fbb->GetSize()};
    dec->pulled.emplace(fb.data, std::move(pulled));
    *out_fb = fb;  // only once the buffer is recorded, so a failure leaves *out_fb empty
    return OVT_OK;
  });
}

ovt_status aom_plane(ovt_AnalysisDecoder* dec, const char* blob_id, const uint8_t** data, size_t* size) noexcept {
  if (dec == nullptr || blob_id == nullptr || data == nullptr || size == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *data = nullptr;
  *size = 0;
  return guarded([&] {
    for (const auto& [ptr, pulled] : dec->pulled) {
      if (const auto it = pulled->blobs.find(blob_id); it != pulled->blobs.end()) {
        *data = it->second->data.data();
        *size = it->second->data.size();
        return OVT_OK;
      }
    }
    return OVT_E_INVALID_ARG;  // unknown, or its FrameAnalysis was already released
  });
}

void aom_release(ovt_AnalysisDecoder* dec, ovt_Buffer fb) noexcept {
  if (dec != nullptr) {
    dec->pulled.erase(fb.data);
  }
}

ovt_status aom_flush(ovt_AnalysisDecoder* dec) noexcept {
  if (dec == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] { return dec->decoder->flush(); });
}

const ovt_AnalysisDecoderApi kApi = {
    sizeof(ovt_AnalysisDecoderApi),
    OVT_ANALYSIS_ABI_VERSION,
    aom_caps,
    aom_create,
    aom_destroy,
    aom_push,
    aom_pull,
    aom_plane,
    aom_release,
    aom_flush,
};

}  // namespace

extern "C" OVT_PLUGIN_EXPORT const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void) {
  return &kApi;
}
