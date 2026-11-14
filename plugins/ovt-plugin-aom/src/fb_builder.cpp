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
#include <vector>

#include "map_mi.hpp"
#include "ovt_model_generated.h"
#include "planes.hpp"

namespace ovt::aom {

namespace {

constexpr uint8_t kSeverityInfo = 0;  // ovt.fb.Severity
constexpr uint8_t kSeverityWarning = 1;

}  // namespace

std::unique_ptr<flatbuffers::FlatBufferBuilder> build_frame_analysis(const Frame& f, uint32_t plane_stage_mask,
                                                                     const char* plugin_id, const char* plugin_version,
                                                                     std::map<std::string, PlaneView>* blobs) {
  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(1024);

  // Block tree, grid, MVs, refs and tiles first: the mapping may add warnings.
  const MappedBlocks mapped = map_blocks(*fbb, f.blocks, f.coded_w, f.coded_h);

  std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> diags;
  diags.reserve(f.diagnostics.size() + mapped.warnings.size() + 1);
  for (const Diag& d : f.diagnostics) {
    diags.push_back(ovt::fb::CreateDiagnosticDirect(*fbb, static_cast<ovt::fb::Severity>(d.severity), d.code.c_str(),
                                                    d.message.c_str()));
  }
  // SPEC-ASSUMPTION(#261): no Diagnostic code is registered for these yet; the code stays empty.
  for (const std::string& w : mapped.warnings) {
    diags.push_back(
        ovt::fb::CreateDiagnosticDirect(*fbb, static_cast<ovt::fb::Severity>(kSeverityWarning), "", w.c_str()));
  }
  // Chapter 06 §6.6.1 rule 5: Reconstruction is the picture before film grain synthesis (AV1D_SET_SKIP_FILM_GRAIN).
  // SPEC-ASSUMPTION(#389): §7.6.1's derived note "film grain present (not applied)" has no field in
  // FrameAnalysis, so it is an Info Diagnostic on the frame, with the DEC-AV1 Info code the #389 triage asks the
  // §7.9.4 registry to add (V-029 rejects an empty code).
  if (f.apply_grain) {
    diags.push_back(ovt::fb::CreateDiagnosticDirect(*fbb, static_cast<ovt::fb::Severity>(kSeverityInfo), "DEC-AV1-0001",
                                                    "film grain present (not applied)"));
  }
  const auto diags_v = fbb->CreateVector(diags);

  // Hidden frames have no output image, so no planes without the snapshot patch (the host adds ANA-0001).
  std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>> planes;
  add_plane_refs(*fbb, f.decode_index, f.recon, plane_stage_mask, &planes, blobs);
  // Extension patch 0002: the frame before each in-loop filter, hidden frames included (WI-2.159).
  constexpr std::array<ovt::fb::PlaneStage, 3> kSnapshotStages = {
      ovt::fb::PlaneStage::PreDeblock, ovt::fb::PlaneStage::PreSaoOrCdef, ovt::fb::PlaneStage::PreLoopRestoration};
  for (size_t k = 0; k < kSnapshotStages.size(); ++k) {
    add_stage_plane_refs(*fbb, f.decode_index, kSnapshotStages.at(k), f.snapshots.at(k), plane_stage_mask, &planes,
                         blobs);
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
