// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libovt_plugin_mock: a tiny H264 analysis plugin for the plugin-host and GUI tests (WI-2.150 step 4). Like every
// plugin it links no part of libovt-core (chapter 0 §0.6.2). Synthetic behaviour:
//  - push() completes a frame for its decode_index_hint at once (no frame delay); consecutive pushes with the same
//    hint add their unit to that frame until it is pulled; a unit starting with 0xFF returns OVT_E_CORRUPT;
//  - grid: cells_w = 1 + (total bytes % 16), cells_h = unit count, qp[i] = (total + i) % 52, Σ bits_x8 = total × 8;
//  - no planes. Knobs for the test variants: OVT_MOCK_ID, OVT_MOCK_VERSION, OVT_MOCK_ABI_VERSION, OVT_MOCK_CODEC,
//    OVT_MOCK_API_SIZE (struct_size), OVT_MOCK_NULL_FLUSH (a NULL flush entry).

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <new>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt_model_generated.h"

#ifndef OVT_MOCK_ID
#define OVT_MOCK_ID "mock"
#endif
#ifndef OVT_MOCK_VERSION
#define OVT_MOCK_VERSION "1.0.0"
#endif
#ifndef OVT_MOCK_ABI_VERSION
#define OVT_MOCK_ABI_VERSION OVT_ANALYSIS_ABI_VERSION
#endif
#ifndef OVT_MOCK_CODEC
#define OVT_MOCK_CODEC 1 /* ovt.fb.Codec.H264 */
#endif
#ifndef OVT_MOCK_API_SIZE
#define OVT_MOCK_API_SIZE sizeof(ovt_AnalysisDecoderApi)
#endif

struct ovt_AnalysisDecoder {
  struct Frame {
    int32_t decode_index = 0;
    int64_t pts = 0;
    std::vector<std::size_t> unit_sizes;
  };
  std::deque<Frame> pending;                                                      // completed frames not yet pulled
  std::map<const uint8_t*, std::unique_ptr<flatbuffers::FlatBufferBuilder>> out;  // pulled, not yet released
};

namespace {

const ovt_AnalysisCaps kCaps = {
    sizeof(ovt_AnalysisCaps),
    static_cast<uint8_t>(OVT_MOCK_CODEC),
    0U,                              // plane_stages: none
    OVT_GRID_QP | OVT_GRID_BITS_X8,  // grid_fields
    0,                               // has_symbol_accounting
    1,                               // bit_exact: synthetic output is deterministic
    0,                               // supports_threads
    OVT_MOCK_ID,
    OVT_MOCK_VERSION,
    "LGPL-2.1-or-later",
    "synthetic test plugin (core/tests/analysis/mock_plugin)",
};

const ovt_AnalysisCaps* mock_caps() noexcept {
  return &kCaps;
}

// codec_private is valid only during create(); the mock needs none of it.
ovt_status mock_create(const uint8_t* /*codec_private*/, size_t /*len*/, int /*threads*/,
                       ovt_AnalysisDecoder** out) noexcept {
  *out = new (std::nothrow) ovt_AnalysisDecoder();
  return *out != nullptr ? OVT_OK : OVT_E_NOMEM;
}

void mock_destroy(ovt_AnalysisDecoder* dec) noexcept {
  delete dec;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status mock_push(ovt_AnalysisDecoder* dec, const uint8_t* unit, size_t len, int32_t decode_index_hint,
                     int64_t pts) noexcept {
  if (len > 0 && unit[0] == 0xFF) {
    return OVT_E_CORRUPT;
  }
  try {
    if (dec->pending.empty() || dec->pending.back().decode_index != decode_index_hint) {
      dec->pending.push_back({decode_index_hint, pts, {}});
    }
    dec->pending.back().unit_sizes.push_back(len);
  } catch (...) {
    return OVT_E_NOMEM;
  }
  return OVT_OK;
}

std::unique_ptr<flatbuffers::FlatBufferBuilder> build(const ovt_AnalysisDecoder::Frame& f) {
  std::size_t total = 0;
  for (std::size_t s : f.unit_sizes) {
    total += s;
  }
  const auto cells_w = static_cast<uint32_t>(1 + (total % 16));
  const auto cells_h = static_cast<uint32_t>(f.unit_sizes.size());
  const std::size_t cells = static_cast<std::size_t>(cells_w) * cells_h;
  std::vector<int16_t> qp(cells);
  std::vector<uint32_t> bits(cells, static_cast<uint32_t>(total * 8 / cells));
  for (std::size_t i = 0; i < cells; ++i) {
    qp[i] = static_cast<int16_t>((total + i) % 52);
  }
  bits[0] += static_cast<uint32_t>(total * 8 % cells);

  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(1024);
  const auto qp_v = fbb->CreateVector(qp);
  const auto bits_v = fbb->CreateVector(bits);
  ovt::fb::CellGridBuilder grid(*fbb);
  grid.add_cells_w(cells_w);
  grid.add_cells_h(cells_h);
  grid.add_qp(qp_v);
  grid.add_bits_x8(bits_v);
  const auto grid_off = grid.Finish();

  const ovt::fb::FrameId id(f.decode_index, -1);
  const ovt::fb::Rect coded(0, 0, static_cast<int32_t>(cells_w * 4), static_cast<int32_t>(cells_h * 4));
  ovt::fb::FrameInfoBuilder info(*fbb);
  info.add_id(&id);
  info.add_pts(f.pts);
  info.add_coded_size(&coded);
  info.add_crop(&coded);
  const auto info_off = info.Finish();

  const auto plugin_id = fbb->CreateString(kCaps.id);
  const auto plugin_version = fbb->CreateString(kCaps.version);
  ovt::fb::FrameAnalysisBuilder fa(*fbb);
  fa.add_schema_version(1);
  fa.add_plugin_id(plugin_id);
  fa.add_plugin_version(plugin_version);
  fa.add_frame(info_off);
  fa.add_grid(grid_off);
  ovt::fb::FinishFrameAnalysisBuffer(*fbb, fa.Finish());
  return fbb;
}

ovt_status mock_pull(ovt_AnalysisDecoder* dec, uint32_t /*plane_stage_mask*/, ovt_Buffer* out_fb) noexcept {
  *out_fb = ovt_Buffer{nullptr, 0};
  if (dec->pending.empty()) {
    return OVT_OK;
  }
  try {
    auto fbb = build(dec->pending.front());
    dec->pending.pop_front();
    *out_fb = ovt_Buffer{fbb->GetBufferPointer(), fbb->GetSize()};
    dec->out.emplace(out_fb->data, std::move(fbb));
  } catch (...) {
    return OVT_E_NOMEM;
  }
  return OVT_OK;
}

ovt_status mock_plane(ovt_AnalysisDecoder* /*dec*/, const char* /*blob_id*/, const uint8_t** data,
                      size_t* size) noexcept {
  *data = nullptr;
  *size = 0;
  return OVT_E_INVALID_ARG;  // the mock exports no planes
}

void mock_release(ovt_AnalysisDecoder* dec, ovt_Buffer fb) noexcept {
  dec->out.erase(fb.data);
}

[[maybe_unused]] ovt_status mock_flush(ovt_AnalysisDecoder* /*dec*/) noexcept {
  return OVT_OK;  // no frame delay: nothing is held back
}

const ovt_AnalysisDecoderApi kApi = {
    static_cast<uint32_t>(OVT_MOCK_API_SIZE),
    OVT_MOCK_ABI_VERSION,
    mock_caps,
    mock_create,
    mock_destroy,
    mock_push,
    mock_pull,
    mock_plane,
    mock_release,
#ifdef OVT_MOCK_NULL_FLUSH
    nullptr,
#else
    mock_flush,
#endif
};

}  // namespace

extern "C" OVT_PLUGIN_EXPORT const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void) {
  return &kApi;
}
