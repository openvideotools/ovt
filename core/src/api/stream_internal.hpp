// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ovt_Stream handle behind ovt/index.h (WI-1.121, WI-1.125). Module-internal: tests that link the module objects
// include it to read the handle's state or to lower the lazy-tree threshold (chapter 7 §7.4.5) on a short stream.

#ifndef OVT_SRC_API_STREAM_INTERNAL_HPP
#define OVT_SRC_API_STREAM_INTERNAL_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "index/rap_map.h"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/index.h"
#include "ovt_model_generated.h"

namespace ovt::api {

enum class Framing : uint8_t { AnnexB, ObuSection5, ObuAnnexB, Ivf };

/// §7.4.5 default: VCL units of frames from this decode index on get no header tree.
inline constexpr uint32_t kDefaultLazyAfterFrames = 10000;

/// The builder of a partial index (OVT_INDEX_ALLOW_PARTIAL, §7.4.6), kept so that the next build resumes: the
/// first `units_fed` units of the scan are skipped and the rest go into the same builder.
struct ResumeState {
  std::unique_ptr<index::StreamIndexBuilder> builder;
  uint32_t units_fed = 0;
  bool scan_complete = false;  // the cancelled pass fed every unit (Driver::end() threw): nothing is left to scan
};

}  // namespace ovt::api

struct ovt_Stream {
  ovt_Container* container = nullptr;  // not owned; outlives the stream
  ovt::fb::Codec codec = ovt::fb::Codec::Unknown;
  ovt::api::Framing framing = ovt::api::Framing::AnnexB;
  flatbuffers::DetachedBuffer index;  // the last built StreamIndex (ovt_stream_index()), the builder's own buffer
  ovt::index::RapMap raps;            // §7.4.2 S11
  bool built = false;
  uint32_t index_flags = 0;                                        // ovt_IndexFlags
  uint32_t lazy_after_frames = ovt::api::kDefaultLazyAfterFrames;  // BuilderOptions.lazy_after_frames
  std::unique_ptr<ovt::api::ResumeState> resume;                   // set after a partial index
};

#endif  // OVT_SRC_API_STREAM_INTERNAL_HPP
