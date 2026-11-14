// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// PlaneRef export of ovt-plugin-de265 (chapter 07 §7.6.2, chapter 06 §6.6.1, WI-2.166). See planes.hpp.

#include "planes.hpp"

#ifdef OVT_DE265_INTERNALS
#include <libde265/de265.h>
#include <libde265/de265_internals.h>
#endif

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

namespace ovt::de265 {
namespace {

// Chapter 06 §6.6.2 stage tokens, in ovt.fb.PlaneStage order.
constexpr std::array<const char*, 7> kStageTokens = {"recon",      "pred",  "resid", "predeblock",
                                                     "presaocdef", "prelr", "coeff"};
constexpr std::array<const char*, 3> kPlaneNames = {"y", "u", "v"};
// The patch-0003 stages in PlaneStage order.
constexpr std::array<uint32_t, 5> kSnapshotStages = {kStagePrediction, kStageResidual, kStagePreDeblock, kStagePreSao,
                                                     kStageCoefficients};

// SPEC-ASSUMPTION(#450): the dynamic range of dequantised HEVC coefficients (CoeffMinY/C .. CoeffMaxY/C = −2^15 .. 2^15
// − 1 without extended_precision_processing, H.265 §7.4.3.3.2), reported as the Coefficients planes' bit_depth.
constexpr uint8_t kCoefficientBits = 16;

}  // namespace

std::string blob_id(int32_t decode_index, uint32_t stage, int plane) {
  return std::to_string(decode_index) + "/" + kStageTokens.at(stage) + "/" +
         kPlaneNames.at(static_cast<std::size_t>(plane));
}

#ifdef OVT_DE265_INTERNALS
namespace {

// libde265's copy of one snapshot plane: rows of width × sample_bytes bytes, no padding.
struct Snapshot {
  std::span<const uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
};

Snapshot snapshot(const de265_image* img, int which, int c, std::size_t sample_bytes) {
  int w = 0;
  int h = 0;
  int stride = 0;
  const void* data = de265_internals_get_snapshot_plane(img, which, c, &w, &h, &stride);
  if (data == nullptr || w <= 0 || h <= 0 ||
      static_cast<std::size_t>(stride) != static_cast<std::size_t>(w) * sample_bytes) {
    return {};
  }
  const std::size_t size = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
  return Snapshot{std::span<const uint8_t>(static_cast<const uint8_t*>(data), size), static_cast<uint32_t>(w),
                  static_cast<uint32_t>(h)};
}

Plane plane_of(const Snapshot& s, uint8_t bit_depth, uint8_t sample_bytes, bool is_signed) {
  Plane p;
  p.data.assign(s.bytes.begin(), s.bytes.end());
  p.width = s.width;
  p.height = s.height;
  p.bit_depth = bit_depth;
  p.sample_bytes = sample_bytes;
  p.is_signed = is_signed;
  return p;
}

uint16_t sample_at(std::span<const uint8_t> bytes, std::size_t i, std::size_t sample_bytes) {
  if (sample_bytes == 1) {
    return bytes[i];
  }
  uint16_t v = 0;
  std::memcpy(&v, bytes.subspan(i * 2, 2).data(), sizeof v);  // native order: every target is little-endian
  return v;
}

// Residual = PreDeblock − Prediction as int16 little-endian. SPEC-ASSUMPTION(#450): after the decoder's clipping,
// so that Prediction + Residual == PreDeblock exactly (WI-2.166 AT2).
Plane residual_of(const Snapshot& pre_deblock, const Snapshot& prediction, uint8_t bit_depth,
                  std::size_t sample_bytes) {
  Plane p;
  p.width = pre_deblock.width;
  p.height = pre_deblock.height;
  p.bit_depth = bit_depth;
  p.sample_bytes = 2;
  p.is_signed = true;
  const std::size_t samples = std::size_t{p.width} * p.height;
  p.data.resize(samples * 2);
  for (std::size_t i = 0; i < samples; ++i) {
    const auto r = static_cast<int16_t>(static_cast<int>(sample_at(pre_deblock.bytes, i, sample_bytes)) -
                                        static_cast<int>(sample_at(prediction.bytes, i, sample_bytes)));
    std::memcpy(&p.data[i * 2], &r, sizeof r);
  }
  return p;
}

}  // namespace

bool copy_snapshots(const de265_image* img, uint32_t stages, Frame* frame) {
  stages &= kSnapshotStageMask;
  bool complete = true;
  for (int c = 0; c < 3 && stages != 0; ++c) {
    const Plane& recon = frame->planes.at(static_cast<std::size_t>(c));
    if (recon.data.empty()) {
      continue;
    }
    const std::size_t bytes = recon.sample_bytes;
    const Snapshot pre_deblock = snapshot(img, DE265_INTERNALS_SNAPSHOT_PRE_DEBLOCK, c, bytes);
    const Snapshot pre_sao = snapshot(img, DE265_INTERNALS_SNAPSHOT_PRE_SAO, c, bytes);
    const Snapshot prediction = snapshot(img, DE265_INTERNALS_SNAPSHOT_PREDICTION, c, bytes);
    const Snapshot coefficients = snapshot(img, DE265_INTERNALS_SNAPSHOT_COEFFICIENTS, c, sizeof(int32_t));
    const auto put = [&](uint32_t stage, const Snapshot& s, Plane plane) {
      if ((stages & (1U << stage)) == 0 || s.bytes.empty()) {
        return;
      }
      if (s.width != recon.width || s.height != recon.height) {
        complete = false;  // left out rather than exported misaligned with Reconstruction
        return;
      }
      frame->snapshots[stage].at(static_cast<std::size_t>(c)) = std::move(plane);
    };
    put(kStagePreDeblock, pre_deblock, plane_of(pre_deblock, recon.bit_depth, recon.sample_bytes, false));
    put(kStagePreSao, pre_sao, plane_of(pre_sao, recon.bit_depth, recon.sample_bytes, false));
    put(kStagePrediction, prediction, plane_of(prediction, recon.bit_depth, recon.sample_bytes, false));
    put(kStageCoefficients, coefficients, plane_of(coefficients, kCoefficientBits, sizeof(int32_t), true));
    if ((stages & (1U << kStageResidual)) != 0 && !pre_deblock.bytes.empty() && !prediction.bytes.empty()) {
      put(kStageResidual, pre_deblock, residual_of(pre_deblock, prediction, recon.bit_depth, bytes));
    }
  }
  de265_internals_release_snapshots(img);
  if (!complete) {
    frame->snapshots.clear();
  }
  return complete;
}
#endif

void keep_stages(Frame* frame, uint32_t plane_stage_mask) {
  std::erase_if(frame->snapshots,
                [plane_stage_mask](const auto& entry) { return (plane_stage_mask & (1U << entry.first)) == 0; });
}

void add_plane_refs(flatbuffers::FlatBufferBuilder& fbb, const Frame& frame, uint32_t plane_stage_mask,
                    std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>>* out,
                    std::map<std::string, const Plane*>* blobs) {
  const auto add_stage = [&](uint32_t stage, const StagePlanes& planes) {
    for (int c = 0; c < 3; ++c) {
      const Plane& p = planes.at(static_cast<std::size_t>(c));
      if (p.data.empty()) {
        continue;
      }
      std::string id = blob_id(frame.decode_index, stage, c);
      const auto id_off = fbb.CreateString(id);
      ovt::fb::PlaneRefBuilder pb(fbb);
      pb.add_stage(static_cast<ovt::fb::PlaneStage>(stage));
      pb.add_plane(static_cast<uint8_t>(c));
      pb.add_width(p.width);
      pb.add_height(p.height);
      pb.add_stride_bytes(p.width * p.sample_bytes);
      pb.add_bit_depth(p.bit_depth);
      pb.add_sample_bytes(p.sample_bytes);
      pb.add_is_signed(p.is_signed);
      pb.add_blob_id(id_off);
      out->push_back(pb.Finish());
      blobs->emplace(std::move(id), &p);
    }
  };
  if (!frame.picture) {
    return;
  }
  if ((plane_stage_mask & (1U << kStageRecon)) != 0) {
    add_stage(kStageRecon, frame.planes);
  }
  // In PlaneStage order, so the planes[] vector does not depend on how the map was filled.
  for (const uint32_t stage : kSnapshotStages) {
    if ((plane_stage_mask & (1U << stage)) == 0) {
      continue;
    }
    if (const auto it = frame.snapshots.find(stage); it != frame.snapshots.end()) {
      add_stage(stage, it->second);
    }
  }
}

}  // namespace ovt::de265
