// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The DASH MPD parser and segment URL expansion (chapter 08 §8.6.7 / §8.10, WI-4.208). parse() turns the text of
// an MPD into periods -> adaptation sets -> representations with a fully expanded segment list (ISO/IEC 23009-1
// §5.3), and write_manifest() serialises that into the `Manifest` table of ovt_container.fbs.
// SPEC-ASSUMPTION(#349): Period start/duration are milliseconds (-1 unknown); SegmentTemplate segments carry the
// media time presentationTimeOffset + k * duration in `start_time`; the CNT-DASH-nnnn diagnostic family. SegmentList,
// SegmentBase (sidx through an injected resolver) and ContentProtection arrive with WI-4.208 part b.

#ifndef OVT_CONTAINER_VIRTUAL_FILE_MPD_HPP
#define OVT_CONTAINER_VIRTUAL_FILE_MPD_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "ovt_container_generated.h"

namespace ovt::container::mpd {

struct ByteRange {
  uint64_t offset = 0;
  uint64_t length = 0;  // 0 = whole resource
};

struct Segment {
  std::string url;  // absolute when the manifest URL is, otherwise as resolved against it
  ByteRange byte_range;
  int64_t start_time = -1;  // representation timescale; -1 unknown
  int64_t duration = -1;    // representation timescale; -1 unknown
  int64_t number = -1;
  bool is_init = false;
};

struct Representation {
  std::string id;
  uint32_t bandwidth = 0;
  std::string codecs;
  std::string mime_type;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string frame_rate;
  uint32_t timescale = 1;
  std::vector<Segment> segments;  // init segment first (is_init), then media segments in order
};

struct AdaptationSet {
  std::string id;
  std::string content_type;  // "video", "audio", "text", ... (contentType, else the mimeType prefix)
  std::string lang;
  std::vector<Representation> representations;
};

struct Period {
  std::string id;
  int64_t start_ms = -1;
  int64_t duration_ms = -1;
  std::vector<AdaptationSet> adaptation_sets;
};

struct Diagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;  // byte offset in the MPD text (0 when not known)
};

struct Manifest {
  std::string url;
  bool dynamic = false;
  std::vector<Period> periods;
  std::vector<Diagnostic> diagnostics;
};

struct Options {
  std::string manifest_url;  // base for relative BaseURLs / segment URLs; may be a path
};

struct ParseResult {
  bool ok = false;  // false: not a well-formed MPD; manifest.diagnostics holds CNT-DASH-0009
  Manifest manifest;
};

[[nodiscard]] ParseResult parse(std::string_view xml, const Options& options);

// RFC 3986 §5 reference resolution (exposed for tests). An empty `base` returns `ref` with dot segments removed.
[[nodiscard]] std::string resolve_url(std::string_view base, std::string_view ref);

// Expands the identifiers of a SegmentTemplate attribute (`$$`, `$RepresentationID$`, `$Number[%0Nd]$`,
// `$Bandwidth[%0Nd]$`, `$Time[%0Nd]$`). nullopt when an identifier or format tag is invalid.
struct TemplateValues {
  std::string_view representation_id;
  int64_t number = 0;
  int64_t bandwidth = 0;
  int64_t time = 0;
};
[[nodiscard]] std::optional<std::string> expand_template(std::string_view tmpl, const TemplateValues& values);

[[nodiscard]] flatbuffers::Offset<fb::Manifest> write_manifest(flatbuffers::FlatBufferBuilder& fbb,
                                                               const Manifest& manifest);

}  // namespace ovt::container::mpd

#endif  // OVT_CONTAINER_VIRTUAL_FILE_MPD_HPP
