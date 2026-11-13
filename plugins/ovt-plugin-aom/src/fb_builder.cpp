// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// FrameAnalysis FlatBuffer of one AV1 frame (chapter 07 §7.6.1, WI-2.155; block data WI-2.156).

#include "fb_builder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "map_mi.hpp"
#include "ovt_model_generated.h"

namespace ovt::aom {

namespace {

constexpr uint32_t kStageRecon = 0;      // ovt.fb.PlaneStage.Reconstruction
constexpr uint8_t kSeverityWarning = 1;  // ovt.fb.Severity
constexpr std::array<const char*, 3> kPlaneNames = {"y", "u", "v"};

}  // namespace

std::string blob_id(int32_t decode_index, int plane) {
  return std::to_string(decode_index) + "/recon/" + kPlaneNames.at(static_cast<std::size_t>(plane));
}

std::unique_ptr<flatbuffers::FlatBufferBuilder> build_frame_analysis(const Frame& f, uint32_t plane_stage_mask,
                                                                     const char* plugin_id, const char* plugin_version,
                                                                     std::map<std::string, const Plane*>* blobs) {
  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(1024);

  // Block tree, grid, MVs, refs and tiles first: the mapping may add warnings.
  const MappedBlocks mapped = map_blocks(*fbb, f.blocks, f.coded_w, f.coded_h);

  std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> diags;
  diags.reserve(f.diagnostics.size() + mapped.warnings.size());
  for (const Diag& d : f.diagnostics) {
    diags.push_back(ovt::fb::CreateDiagnosticDirect(*fbb, static_cast<ovt::fb::Severity>(d.severity), d.code.c_str(),
                                                    d.message.c_str()));
  }
  // SPEC-ASSUMPTION(#261): no Diagnostic code is registered for these yet; the code stays empty.
  for (const std::string& w : mapped.warnings) {
    diags.push_back(
        ovt::fb::CreateDiagnosticDirect(*fbb, static_cast<ovt::fb::Severity>(kSeverityWarning), "", w.c_str()));
  }
  const auto diags_v = fbb->CreateVector(diags);

  // Hidden frames have no output image, so no planes without the snapshot patch (the host adds ANA-0001).
  std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>> planes;
  if ((plane_stage_mask & (1U << kStageRecon)) != 0) {
    for (int c = 0; c < 3; ++c) {
      const Plane& p = f.planes.at(static_cast<std::size_t>(c));
      if (p.data.empty()) {
        continue;
      }
      std::string id = blob_id(f.decode_index, c);
      planes.push_back(ovt::fb::CreatePlaneRefDirect(*fbb, ovt::fb::PlaneStage::Reconstruction, static_cast<uint8_t>(c),
                                                     p.width, p.height, p.width * p.sample_bytes, p.bit_depth,
                                                     p.sample_bytes, /*is_signed=*/false, id.c_str()));
      blobs->emplace(std::move(id), &p);
    }
  }
  const auto planes_v = fbb->CreateVector(planes);
  const auto filter_type_off =
      mapped.filter_params.IsNull() ? flatbuffers::Offset<flatbuffers::String>() : fbb->CreateString("Av1FilterParams");

  // insp_frame_data has no order_hint, frame id or tile-group count: the host index fills them (§7.5.3 A6).
  constexpr uint8_t kKey = 3;  // ovt.fb.FrameType.Key
  const ovt::fb::FrameId id(f.decode_index, -1);
  const ovt::fb::Rect coded(0, 0, f.coded_w, f.coded_h);
  const ovt::fb::Rect crop(0, 0, f.render_w, f.render_h);  // the render size, a display hint (§6.2.1 rule 3)
  ovt::fb::FrameInfoBuilder fi(*fbb);
  fi.add_id(&id);
  fi.add_pts(f.pts);
  fi.add_type(static_cast<ovt::fb::FrameType>(f.frame_type));
  fi.add_shown(f.shown);
  fi.add_is_rap(f.frame_type == kKey && f.shown);
  fi.add_coded_size(&coded);
  fi.add_crop(&crop);
  fi.add_chroma_format(f.chroma_format);
  fi.add_bit_depth_luma(f.bit_depth);
  fi.add_bit_depth_chroma(f.bit_depth);
  fi.add_qp_base(f.qp_base);
  const auto info_off = fi.Finish();

  const auto plugin_id_off = fbb->CreateString(plugin_id);
  const auto plugin_version_off = fbb->CreateString(plugin_version);
  ovt::fb::FrameAnalysisBuilder fa(*fbb);
  fa.add_schema_version(1);
  fa.add_plugin_id(plugin_id_off);
  fa.add_plugin_version(plugin_version_off);
  fa.add_frame(info_off);
  fa.add_grid(mapped.grid);
  fa.add_blocks(mapped.blocks);
  fa.add_mvs(mapped.mvs);
  fa.add_refs(mapped.refs);
  fa.add_partitions(mapped.partitions);
  fa.add_filter_params(mapped.filter_params);
  fa.add_filter_params_type(filter_type_off);
  fa.add_planes(planes_v);
  fa.add_diagnostics(diags_v);
  ovt::fb::FinishFrameAnalysisBuffer(*fbb, fa.Finish());
  return fbb;
}

}  // namespace ovt::aom
