// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::analysis::Session: the GOP-local analysis driver behind one ovt_Analysis handle (chapter 7 §7.5.2–§7.5.4,
// WI-2.151). Internal to libovt-core. WI-2.151b adds the C entry points of §0.6.1 (core/src/api/analysis_api.cpp)
// once ovt_Stream exists, with the plane path (PlaneRef planes copied into the PictureStore, ovt_analysis_plane).
//
// SPEC-ASSUMPTION(#253): the Session reads the StreamIndex FlatBuffer (ovt.fb.StreamIndex) and the stream's file
// instead of the index module's C++ objects, so the ActiveSets snapshot at R is derived from
// StreamIndex.parameter_sets; the cache is reached through the SessionCache seam that WI-2.152 fills with
// FrameCache; host diagnostics use the codes ANA-0002..ANA-0010 proposed in the issue.

#ifndef OVT_SRC_ANALYSIS_SESSION_HPP
#define OVT_SRC_ANALYSIS_SESSION_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt/status.h"
#include "ovt_model_generated.h"

namespace ovt::analysis {

// A diagnostic raised by the host while serving one frame() call (§7.9.5 codes).
struct HostDiagnostic {
  ovt::fb::Severity severity = ovt::fb::Severity::Info;
  std::string code;
  std::string message;
};

// What SessionCache::lookup returns: a FrameAnalysis buffer (not size-prefixed) and the PlaneStage bits whose
// planes the cache also holds for that frame.
struct CachedAnalysis {
  std::vector<uint8_t> frame_analysis;
  uint32_t stage_mask = 0;
};

// The cache seam of §7.5.5. The key's source hash and plugin id/version are fixed per Session, so only the decode
// index varies; WI-2.152 implements this over FrameCache. Implementations never throw: a failure is a miss
// (lookup) or a logged no-op (store).
class SessionCache {
 public:
  SessionCache() = default;
  SessionCache(const SessionCache&) = delete;
  SessionCache& operator=(const SessionCache&) = delete;
  SessionCache(SessionCache&&) = delete;
  SessionCache& operator=(SessionCache&&) = delete;
  virtual ~SessionCache() = default;
  virtual std::optional<CachedAnalysis> lookup(uint32_t decode_index) noexcept = 0;
  virtual void store(uint32_t decode_index, std::span<const uint8_t> frame_analysis, uint32_t stage_mask) noexcept = 0;
};

struct SessionOptions {
  std::vector<uint8_t> codec_private;  // avcC/hvcC/av1C from the container; empty for elementary streams
  int threads = 0;                     // 0 = min(4, hardware threads) (2 on Android), §7.5.3 A3
  SessionCache* cache = nullptr;       // not owned; nullptr = no caching (--no-cache)
};

struct FrameRequest {
  // Decode from the RAP before rap_before(T) (§7.5.3 A4 `force_rap`): the GUI's "decode from previous RAP" for a
  // RASL picture after a CRA.
  bool from_previous_rap = false;
};

class Session {
 public:
  // `stream_index` is a StreamIndex FlatBuffer (not size-prefixed), copied here; `file` and `plugin` must outlive
  // the Session. Throws std::invalid_argument when the index fails verification or names no codec.
  Session(const ovt::io::RandomAccessFile& file, std::span<const uint8_t> stream_index, const LoadedPlugin& plugin,
          SessionOptions options);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;

  // §7.5.3: the FrameAnalysis of decode index T in `out` (a finished, not size-prefixed buffer). Not thread-safe;
  // one job at a time (§7.5.4). `cancel` may be null. Diagnostics of the call are in diagnostics().
  ovt_status frame(int decode_index, uint32_t plane_stage_mask, const ovt::jobs::Cancel* cancel,
                   std::vector<uint8_t>& out, FrameRequest request = {});

  // Diagnostics raised by the last frame() call, in order.
  [[nodiscard]] const std::vector<HostDiagnostic>& diagnostics() const noexcept { return diagnostics_; }

  // Decoder state, for the tests.
  [[nodiscard]] bool has_decoder() const noexcept { return dec_ != nullptr; }
  [[nodiscard]] int last_decoded() const noexcept { return last_decoded_; }

 private:
  struct Pulled {
    std::vector<uint8_t> fb;  // the target's FrameAnalysis bytes, copied before release()
    bool have = false;
  };

  ovt_status frame_impl(int t, uint32_t stage_mask, const ovt::jobs::Cancel* cancel, std::vector<uint8_t>& out,
                        FrameRequest request);
  ovt_status decode_target(int t, uint32_t stage_mask, const ovt::jobs::Cancel* cancel, std::vector<uint8_t>& out,
                           FrameRequest request);
  ovt_status decode_range(int rap, int t, uint32_t stage_mask, const ovt::jobs::Cancel* cancel, Pulled& result);
  [[nodiscard]] int rap_for(int t, bool from_previous_rap) const;
  ovt_status create_decoder(int rap);
  void destroy_decoder() noexcept;
  ovt_status feed_parameter_sets(int rap);
  ovt_status push_unit(uint32_t unit_index, int32_t decode_index, int64_t pts);
  ovt_status drain(int target, uint32_t stage_mask, Pulled& result);
  [[nodiscard]] bool is_rasl_of(int d, int rap) const;
  ovt_status post_process(int t, uint32_t stage_mask, std::vector<uint8_t>& fb);
  void diag(ovt::fb::Severity severity, std::string code, std::string message);
  [[nodiscard]] const ovt::fb::StreamIndex& index() const noexcept { return *index_; }

  const ovt::io::RandomAccessFile& file_;
  std::vector<uint8_t> index_bytes_;
  const ovt::fb::StreamIndex* index_ = nullptr;
  const LoadedPlugin& plugin_;
  SessionOptions options_;
  std::vector<int> rap_before_;  // §7.4.2 S11

  ovt_AnalysisDecoder* dec_ = nullptr;  // current decoder instance, or null
  int last_decoded_ = -1;               // last decode index whose units were all pushed to dec_
  int current_rap_ = -1;                // RAP the current decoder instance started from
  int consecutive_corrupt_ = 0;
  std::vector<HostDiagnostic> diagnostics_;
  std::vector<uint8_t> scratch_;  // unit bytes being pushed
};

// Rewrites one AV1 OBU without obu_has_size_field (Annex B) to the Section 5 form with obu_size (§7.5.3 A4);
// returns the input unchanged when the field is already present. Exposed for the tests.
[[nodiscard]] std::vector<uint8_t> av1_obu_with_size_field(std::span<const uint8_t> obu);

}  // namespace ovt::analysis

#endif  // OVT_SRC_ANALYSIS_SESSION_HPP
