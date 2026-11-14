// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The HLS M3U8 parser (chapter 08 §8.6.7 / §8.10, WI-4.209). parse() turns the text of a master or media playlist
// (RFC 8216 plus the Apple extensions CODECS, SUPPLEMENTAL-CODECS and VIDEO-RANGE) into variants, renditions or a
// segment list, and write_manifest() serialises that into the `Manifest` table of ovt_container.fbs with
// kind "hls-m3u8". Nothing is fetched and nothing is decrypted.
// SPEC-ASSUMPTION(#416): the CNT-HLS-nnnn diagnostic family; times are microseconds (timescale 1000000), start_time
// is the running sum of the preceding #EXTINF durations; one period, one adaptation set per media type; #EXT-X-MAP
// is an is_init segment inserted where the map changes.

#ifndef OVT_CONTAINER_VIRTUAL_FILE_M3U8_HPP
#define OVT_CONTAINER_VIRTUAL_FILE_M3U8_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "container/virtual_file/mpd.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::m3u8 {

using mpd::ByteRange;  // offset/length; length 0 = whole resource
using mpd::Diagnostic;

inline constexpr uint32_t kTimescale = 1'000'000;

struct Encryption {
  std::string method;  // "AES-128", "SAMPLE-AES", ... (#EXT-X-KEY METHOD; NONE is not listed)
  std::string key_uri;
  std::vector<uint8_t> iv;  // 16 bytes, or empty when the IV is implicit (the media sequence number)
  bool operator==(const Encryption&) const = default;
};

struct Segment {
  std::string url;  // absolute when the manifest URL is, otherwise as resolved against it
  ByteRange byte_range;
  int64_t start_time = -1;  // microseconds from the first segment; -1 for an init segment
  int64_t duration = -1;    // microseconds; -1 unknown
  int64_t number = -1;      // media sequence number; -1 for an init segment
  bool is_init = false;
  int32_t key_index = -1;  // index into MediaPlaylist::keys; -1 clear
  uint32_t discontinuity_sequence = 0;
  bool discontinuity = false;     // #EXT-X-DISCONTINUITY precedes this segment
  std::string program_date_time;  // #EXT-X-PROGRAM-DATE-TIME as written, empty when absent
};

struct MediaPlaylist {
  uint32_t version = 1;
  uint32_t target_duration = 0;  // seconds
  uint64_t media_sequence = 0;
  uint32_t discontinuity_sequence = 0;
  bool end_list = false;
  bool independent_segments = false;
  std::string playlist_type;      // "VOD", "EVENT" or empty
  std::vector<Segment> segments;  // each init segment directly before the first segment it applies to
  std::vector<Encryption> keys;   // distinct keys in order of appearance
};

// One #EXT-X-STREAM-INF or #EXT-X-I-FRAME-STREAM-INF.
struct Variant {
  std::string uri;
  bool is_iframe = false;
  uint32_t bandwidth = 0;
  uint32_t average_bandwidth = 0;
  std::string codecs;
  std::string supplemental_codecs;  // Apple: Dolby Vision profile signalling
  uint32_t width = 0;
  uint32_t height = 0;
  std::string frame_rate;
  std::string video_range;  // "SDR", "PQ", "HLG"
  std::string audio;        // GROUP-ID references
  std::string subtitles;
};

// One #EXT-X-MEDIA.
struct Rendition {
  std::string type;  // "AUDIO", "VIDEO", "SUBTITLES", "CLOSED-CAPTIONS"
  std::string group_id;
  std::string name;
  std::string language;
  std::string uri;  // empty when the rendition is muxed into the variants
  bool is_default = false;
  bool autoselect = false;
};

struct Manifest {
  std::string url;
  bool is_master = false;  // true when the text has any #EXT-X-STREAM-INF or #EXT-X-MEDIA
  std::vector<Variant> variants;
  std::vector<Rendition> renditions;
  MediaPlaylist media;  // filled for a media playlist
  std::vector<Diagnostic> diagnostics;
};

struct Options {
  std::string manifest_url;  // base for relative URIs; may be a path
};

struct ParseResult {
  bool ok = false;  // false: not an HLS playlist; manifest.diagnostics holds CNT-HLS-0001
  Manifest manifest;
};

[[nodiscard]] ParseResult parse(std::string_view text, const Options& options);

// Serialises into the shared `Manifest` table. A master playlist gives one representation per variant (no
// segments: media playlists are separate documents); a media playlist gives one representation with its segments.
[[nodiscard]] flatbuffers::Offset<fb::Manifest> write_manifest(flatbuffers::FlatBufferBuilder& fbb,
                                                               const Manifest& manifest);

}  // namespace ovt::container::m3u8

#endif  // OVT_CONTAINER_VIRTUAL_FILE_M3U8_HPP
