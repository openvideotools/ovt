// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The libvmaf host (chapter 9 §9.5; WI-3.309): one ovt_metric_v1 function table per libvmaf model built into
// libvmaf (`vmaf`, `vmaf_neg`, `vmaf_4k`, `vmaf_b` and the further ids of vmaf_models.inc) and per libvmaf feature
// id (`vif`, `ms_ssim`, `psnr_hvs`, `ciede2000`, `cambi`, `vmaf_psnr`), compiled into libovt-core and registered
// with the PluginRegistry as built-ins. Internal header of the metrics module.
//
// One instance owns one VmafContext (one per job, metric and chunk, §9.2.2 rule 1, §9.5.2). Frames must be
// CodeValue planes of equal geometry and bit depth for ref and dist (the normalisation stage guarantees it,
// §9.5.4). libvmaf computes integer_motion2(t) only when frame t+1 arrives, so score() delivers the FrameScore
// of the previous frame (out->data == NULL on the first call, rule 3) and finish() delivers the last frame's.
// Feature ids read no neighbouring frame: score() delivers the frame's own FrameScore and finish() none.
//
// SPEC-ASSUMPTION(#394): finish() returns the per-frame FrameScore of the instance's last frame (frame present);
// libvmaf ids have no sequence-level score (OVT pools them, §9.8).
// SPEC-ASSUMPTION(#394): VmafConfiguration.n_threads = 0 (synchronous extraction) so scores can be delivered frame
// by frame; parallelism comes from the host's chunks.
// SPEC-ASSUMPTION(#394): a chunk overlaps its neighbours by one frame on each side (kLookbehind = kLookahead = 1);
// the overlap frames' scores are discarded.
// SPEC-ASSUMPTION(#460): feature ids need no overlap (overlap() is {0, 0}); `vif` uses libvmaf's integer extractor
// `vif` (or `float_vif`), `cambi` is NoReference only, `vmaf_psnr` has no sequence-level score.

#ifndef OVT_SRC_METRICS_VMAF_VMAF_HOST_H
#define OVT_SRC_METRICS_VMAF_VMAF_HOST_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/abi/metric_v1.h"

namespace ovt::metrics {

class PluginRegistry;

namespace vmaf {

inline constexpr size_t kMaxOutputs = 16;

// One built-in libvmaf model (a row of vmaf_models.inc, chapter 9 §9.5.3).
struct ModelInfo {
  const char* id;                                // OVT metric id
  const char* version;                           // libvmaf built-in model name for vmaf_model_load()
  bool collection;                               // a bootstrapped model collection (vmaf_model_collection_load)
  bool needs_float;                              // compiled in only with -Denable_float=true
  std::array<const char*, kMaxOutputs> outputs;  // FrameScore.values names; unused entries are nullptr
  [[nodiscard]] size_t output_count() const noexcept;
};

// Every model of vmaf_models.inc, sorted by id.
[[nodiscard]] std::span<const ModelInfo> models() noexcept;

// The type of one option of a feature id (passed to libvmaf as a feature dictionary entry).
enum class OptionType : uint8_t { Bool, Int, Double, String };

struct FeatureOption {
  const char* name;
  OptionType type;
  double min = 0.0;  // Int and Double: the range libvmaf accepts (checked here: libvmaf 3.0.0 leaks the extractor
  double max = 0.0;  // context when it rejects an option value)
};

// One libvmaf feature id (chapter 9 §9.5.3): a single libvmaf feature extractor whose per-frame features are the
// FrameScore values.
struct FeatureInfo {
  const char* id;                                     // OVT metric id
  const char* display_name;                           // MetricDescriptor.display_name
  const char* extractor;                              // vmaf_use_feature() name
  const char* float_extractor;                        // with option {"float": true}; nullptr when there is none
  bool no_reference;                                  // MetricKind::NoReference (ref is NULL in score())
  bool higher_is_better;                              //
  double range_min;                                   //
  double range_max;                                   //
  std::array<const char*, kMaxOutputs> outputs;       // FrameScore.values names; unused entries are nullptr
  std::array<const char*, kMaxOutputs> keys;          // feature-collector key of each output
  std::array<const char*, kMaxOutputs> float_keys;    // the same with float_extractor
  std::span<const FeatureOption> options;             // the options passed through to libvmaf
  std::span<const std::array<const char*, 2>> fixed;  // options always set (key, value)
  [[nodiscard]] size_t output_count() const noexcept;
};

// Every feature id, sorted by id.
[[nodiscard]] std::span<const FeatureInfo> features() noexcept;

// The function table of a libvmaf id (model or feature), or nullptr for an id the host does not know.
[[nodiscard]] const ovt_MetricApi* metric_api(std::string_view id) noexcept;

// True when the linked libvmaf has the feature's extractor (the float ones need -Denable_float=true).
[[nodiscard]] bool feature_available(const FeatureInfo& feature) noexcept;

// True when the linked libvmaf has this model built in (absence of a model is not an error, §9.5.3).
[[nodiscard]] bool model_available(const ModelInfo& model) noexcept;

// `"libvmaf <vmaf_version()>"`, the MetricsResult.engine string of every libvmaf id (§9.5.1).
[[nodiscard]] std::string engine();

// Registers the function table of every available model and feature as a built-in metric. Returns the number
// registered.
size_t register_builtin_vmaf(PluginRegistry& registry);

// Frames a libvmaf chunk of a model id needs beyond its kept range (§9.2.3, §9.5.2; see the SPEC-ASSUMPTION above).
inline constexpr uint32_t kLookbehind = 1;
inline constexpr uint32_t kLookahead = 1;

struct Overlap {
  uint32_t lookbehind = 0;
  uint32_t lookahead = 0;
  friend bool operator==(const Overlap&, const Overlap&) = default;
};

// The chunk overlap of a libvmaf id: {kLookbehind, kLookahead} for a model id, {0, 0} for a feature id and for an
// unknown id.
[[nodiscard]] Overlap overlap(std::string_view id) noexcept;

// One chunk of a chunked run: frames [feed_begin, feed_end) are scored by one instance, in order; only the
// scores of [keep_begin, keep_end) are kept.
struct Chunk {
  uint32_t feed_begin = 0;
  uint32_t keep_begin = 0;
  uint32_t keep_end = 0;
  uint32_t feed_end = 0;
  friend bool operator==(const Chunk&, const Chunk&) = default;
};

// Splits `frames` frames into min(chunks, frames) contiguous kept ranges of near-equal size (the first ones one
// frame longer), each fed with up to `lookbehind` frames before and `lookahead` frames after it. Empty for
// frames == 0; chunks == 0 counts as 1.
[[nodiscard]] std::vector<Chunk> plan_chunks(uint32_t frames, uint32_t chunks, uint32_t lookbehind = kLookbehind,
                                             uint32_t lookahead = kLookahead);

}  // namespace vmaf
}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_VMAF_VMAF_HOST_H
