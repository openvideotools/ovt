// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Planes of the HEVC analysis plugin (chapter 07 §7.6.2 "patch 0003 planes" row, chapter 06 §6.6, WI-2.166).
// Reconstruction comes from the decoded picture (decoder.cpp). Patch 0003 of libde265 adds plane snapshots that
// the plugin exports as five more ovt.fb.PlaneStage stages:
//   PreDeblock    the picture before deblocking (unsigned samples, like Reconstruction),
//   PreSaoOrCdef  the picture after deblocking, before SAO,
//   Prediction    the intra/inter prediction; 0 by convention for PCM and cu_transquant_bypass coding units,
//   Residual      PreDeblock − Prediction, int16 (is_signed); so Prediction + Residual == PreDeblock exactly,
//   Coefficients  dequantised coefficients, int32 (is_signed), coefficient (u,v) of a TB at (x0,y0) at sample
//                 (x0+u, y0+v), zero elsewhere (chapter 06 §6.6.1).
// Snapshots cost libde265 about four times the picture size and are taken only for pictures that start decoding
// after a pull() asked for one of these stages (Decoder::request_stages); the plugin copies them out when the
// picture is output and frees libde265's copies at once.
#ifndef OVT_PLUGIN_DE265_PLANES_HPP
#define OVT_PLUGIN_DE265_PLANES_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "decoder.hpp"
#include "ovt_model_generated.h"

struct de265_image;

namespace ovt::de265 {

// ovt.fb.PlaneStage values (core/schema/ovt_model.fbs).
constexpr uint32_t kStageRecon = 0;
constexpr uint32_t kStagePrediction = 1;
constexpr uint32_t kStageResidual = 2;
constexpr uint32_t kStagePreDeblock = 3;
constexpr uint32_t kStagePreSao = 4;
constexpr uint32_t kStageCoefficients = 6;

/// The stages patch 0003 adds.
constexpr uint32_t kSnapshotStageMask = (1U << kStagePrediction) | (1U << kStageResidual) | (1U << kStagePreDeblock) |
                                        (1U << kStagePreSao) | (1U << kStageCoefficients);

/// AnalysisCaps.plane_stages: Reconstruction, plus the patch-0003 stages when the plugin is built against the
/// patched libde265 (not against a system libde265, OVT_SYSTEM_DE265).
#ifdef OVT_DE265_INTERNALS
constexpr uint32_t kSupportedStageMask = (1U << kStageRecon) | kSnapshotStageMask;
#else
constexpr uint32_t kSupportedStageMask = 1U << kStageRecon;
#endif

/// The plugin-local blob id "<decode_index>/<stage token>/<y|u|v>" (chapter 06 §6.6.2). SPEC-ASSUMPTION(#250):
/// the host prefixes the stream's source hash; a plugin is never told the source hash.
std::string blob_id(int32_t decode_index, uint32_t stage, int plane);

#ifdef OVT_DE265_INTERNALS
/// Copies the snapshots of @p img that @p stages (PlaneStage bits) asks for into @p frame->snapshots and frees
/// libde265's copies. Stages the picture has no snapshot for (it started decoding before the request) are left
/// out. @p frame already holds the Reconstruction planes. Returns false when snapshots exist but were left out
/// because the Reconstruction planes are only the cropped picture (decoder.cpp: a picture libde265 allocated
/// itself, which happens only without worker threads on a stream with SAO), so the stages would not line up.
bool copy_snapshots(const de265_image* img, uint32_t stages, Frame* frame);
#endif

/// Drops the snapshot stages @p plane_stage_mask does not ask for, so a pulled frame keeps only what it exports.
void keep_stages(Frame* frame, uint32_t plane_stage_mask);

/// Appends one PlaneRef per plane of every stage of @p plane_stage_mask that @p frame holds, and records each
/// blob id with its plane in @p blobs.
void add_plane_refs(flatbuffers::FlatBufferBuilder& fbb, const Frame& frame, uint32_t plane_stage_mask,
                    std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>>* out,
                    std::map<std::string, const Plane*>* blobs);

}  // namespace ovt::de265

#endif  // OVT_PLUGIN_DE265_PLANES_HPP
