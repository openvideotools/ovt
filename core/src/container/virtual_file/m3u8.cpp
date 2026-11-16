// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The HLS M3U8 parser (chapter 08 §8.10, WI-4.209). See m3u8.hpp.

#include "container/virtual_file/m3u8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/tree_fb.hpp"

namespace ovt::container::m3u8 {
namespace {

// A playlist never expands past this many segments (init entries included), and never records more diagnostics
// than kMaxDiagnostics, so a hostile playlist cannot exhaust memory.
constexpr size_t kMaxSegments = 1'000'000;
constexpr size_t kMaxDiagnostics = 1000;
// #EXTINF above this many seconds is rejected; kMaxSegments of them still sum within int64 microseconds.
constexpr uint64_t kMaxDurationSeconds = 1'000'000;

struct Attr {
  std::string key;
  std::string value;
};

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::optional<uint64_t> parse_uint(std::string_view s) {
  uint64_t v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || end != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

// A decimal-floating-point number of seconds -> microseconds, rounded half up on the seventh digit.
std::optional<int64_t> parse_duration_us(std::string_view s) {
  s = trim(s);
  const size_t dot = s.find('.');
  const std::string_view whole = s.substr(0, dot);
  const std::string_view frac = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
  if (whole.empty() && frac.empty()) {
    return std::nullopt;
  }
  const bool digits =
      std::all_of(whole.begin(), whole.end(),
                  [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) &&
      std::all_of(frac.begin(), frac.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
  if (!digits || whole.size() > 9) {
    return std::nullopt;
  }
  const auto parsed = parse_uint(whole);
  const uint64_t seconds = parsed.value_or(0);  // empty `whole` (".5") is 0 seconds
  if (seconds > kMaxDurationSeconds) {
    return std::nullopt;
  }
  int64_t micros = 0;
  for (size_t i = 0; i < 6; ++i) {
    micros = (micros * 10) + (i < frac.size() ? frac[i] - '0' : 0);
  }
  if (frac.size() > 6 && frac[6] >= '5') {
    ++micros;
  }
  return (static_cast<int64_t>(seconds) * 1'000'000) + micros;
}

// RFC 8216 §4.2 attribute list. `malformed` is set when an item had no `=` or an unterminated quote.
std::vector<Attr> parse_attributes(std::string_view s, bool& malformed) {
  std::vector<Attr> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ',' || s[i] == ' ' || s[i] == '\t')) {
      ++i;
    }
    if (i >= s.size()) {
      break;
    }
    const size_t key_end = s.find_first_of("=,", i);
    if (key_end == std::string_view::npos || s[key_end] == ',') {
      malformed = true;
      i = key_end == std::string_view::npos ? s.size() : key_end + 1;
      continue;
    }
    Attr a;
    a.key = std::string(trim(s.substr(i, key_end - i)));
    i = key_end + 1;
    if (i < s.size() && s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      if (close == std::string_view::npos) {
        malformed = true;
        a.value = std::string(s.substr(i + 1));
        i = s.size();
      } else {
        a.value = std::string(s.substr(i + 1, close - i - 1));
        i = close + 1;
      }
    } else {
      const size_t end = std::min(s.find(',', i), s.size());
      a.value = std::string(trim(s.substr(i, end - i)));
      i = end;
    }
    out.push_back(std::move(a));
  }
  return out;
}

const std::string* find_attr(const std::vector<Attr>& attrs, std::string_view key) {
  for (const Attr& a : attrs) {
    if (a.key == key) {
      return &a.value;
    }
  }
  return nullptr;
}

std::string attr_text(const std::vector<Attr>& attrs, std::string_view key) {
  const std::string* v = find_attr(attrs, key);
  return v != nullptr ? *v : std::string();
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool is_audio_codec(std::string_view c) {
  constexpr std::array<std::string_view, 9> kPrefixes = {"mp4a", "ac-3", "ec-3", "ac-4", "opus",
                                                         "flac", "alac", "mha1", "mhm1"};
  return std::any_of(kPrefixes.begin(), kPrefixes.end(), [c](std::string_view p) { return starts_with(c, p); });
}

// "avc1.64001f,mp4a.40.2" is video; "mp4a.40.2" alone (and no RESOLUTION) is an audio-only variant.
bool codecs_audio_only(std::string_view codecs) {
  if (codecs.empty()) {
    return false;
  }
  size_t i = 0;
  while (i <= codecs.size()) {
    const size_t comma = std::min(codecs.find(',', i), codecs.size());
    if (!is_audio_codec(trim(codecs.substr(i, comma - i)))) {
      return false;
    }
    i = comma + 1;
  }
  return true;
}

struct Parser {
  const Options& options;
  Manifest m;
  Variant pending_variant;
  std::string program_date_time;
  std::string map_url;
  std::string last_range_url;
  std::optional<int64_t> extinf_duration;  // set by #EXTINF; nullopt when the duration was invalid
  std::optional<uint64_t> byterange_offset;
  ByteRange map_range;
  uint64_t line_offset = 0;
  uint64_t byterange_length = 0;
  uint64_t last_range_end = 0;
  int64_t time_us = 0;
  int32_t current_key = -1;
  uint32_t discontinuity_sequence = 0;
  bool capped = false;
  bool saw_media_tags = false;   // EXTINF / segment-level tags
  bool saw_master_tags = false;  // STREAM-INF / MEDIA
  bool target_warned = false;
  bool encrypted_noted = false;
  bool have_extinf = false;
  bool have_byterange = false;
  bool discontinuity = false;
  bool have_stream_inf = false;
  bool map_valid = false;
  bool map_dirty = false;
  bool have_last_range = false;
  bool m_ok = false;

  explicit Parser(const Options& o) : options(o) { m.url = o.manifest_url; }

  void diag(DiagCode code, std::string message) {
    if (m.diagnostics.size() < kMaxDiagnostics) {
      m.diagnostics.push_back({code, std::move(message), line_offset});
    }
  }

  [[nodiscard]] std::string resolve(std::string_view ref) const { return mpd::resolve_url(options.manifest_url, ref); }

  std::vector<Attr> attributes(std::string_view tag, std::string_view value) {
    bool malformed = false;
    auto attrs = parse_attributes(value, malformed);
    if (malformed) {
      diag(DiagCode::CNT_HLS_0002, "malformed attribute list in " + std::string(tag) + "; the bad items are ignored");
    }
    return attrs;
  }

  std::optional<uint32_t> uint32_attr(const std::vector<Attr>& attrs, std::string_view key) {
    const std::string* v = find_attr(attrs, key);
    if (v == nullptr) {
      return std::nullopt;
    }
    const auto n = parse_uint(*v);
    if (!n || *n > std::numeric_limits<uint32_t>::max()) {
      diag(DiagCode::CNT_HLS_0003, "invalid " + std::string(key) + " `" + *v + "` ignored");
      return std::nullopt;
    }
    return static_cast<uint32_t>(*n);
  }

  // "len[@off]"; length 0 is invalid (it would read as "whole resource").
  bool parse_range(std::string_view text, uint64_t& length, std::optional<uint64_t>& offset) {
    const size_t at = text.find('@');
    const auto len = parse_uint(text.substr(0, at));
    std::optional<uint64_t> off;
    if (at != std::string_view::npos) {
      off = parse_uint(text.substr(at + 1));
    }
    if (!len || *len == 0 || (at != std::string_view::npos && !off)) {
      diag(DiagCode::CNT_HLS_0003, "invalid byte range `" + std::string(text) + "` ignored");
      return false;
    }
    length = *len;
    offset = off;
    return true;
  }

  static constexpr bool add_overflows(uint64_t a, uint64_t b) { return a > std::numeric_limits<uint64_t>::max() - b; }

  static int32_t intern_key(Encryption e, std::vector<Encryption>& list) {
    const auto it = std::find(list.begin(), list.end(), e);
    if (it != list.end()) {
      return static_cast<int32_t>(it - list.begin());
    }
    list.push_back(std::move(e));
    return static_cast<int32_t>(list.size() - 1);
  }

  // #EXT-X-KEY / #EXT-X-SESSION-KEY. Returns the key list index, -1 for METHOD=NONE or an unusable tag.
  int32_t key_tag(std::string_view tag, std::string_view value, std::vector<Encryption>& list) {
    const auto attrs = attributes(tag, value);
    const std::string* method = find_attr(attrs, "METHOD");
    if (method == nullptr) {
      diag(DiagCode::CNT_HLS_0003, std::string(tag) + " without METHOD ignored");
      return -1;
    }
    if (*method == "NONE") {
      return -1;
    }
    Encryption e;
    e.method = *method;
    if (const std::string uri = attr_text(attrs, "URI"); !uri.empty()) {
      e.key_uri = resolve(uri);
    }
    if (const std::string* iv = find_attr(attrs, "IV")) {
      e.iv = parse_iv(*iv);
    }
    if (!encrypted_noted) {
      encrypted_noted = true;
      diag(DiagCode::CNT_HLS_0008,
           "segments are encrypted with METHOD=" + e.method + "; they are listed, not decrypted");
    }
    return intern_key(std::move(e), list);
  }

  // "0x" + up to 32 hex digits, left-padded to 16 bytes; empty (and a diagnostic) when invalid.
  std::vector<uint8_t> parse_iv(const std::string& text) {
    std::string_view s = text;
    if (!(starts_with(s, "0x") || starts_with(s, "0X")) || s.size() <= 2 || s.size() > 34) {
      diag(DiagCode::CNT_HLS_0003, "invalid IV `" + text + "` ignored");
      return {};
    }
    s.remove_prefix(2);
    std::vector<uint8_t> bytes(16, 0);
    const size_t digits = s.size();
    for (size_t i = 0; i < digits; ++i) {
      const char c = s[digits - 1 - i];
      unsigned v = 0;
      const std::string_view digit(&c, 1);
      const bool valid = std::from_chars(digit.begin(), digit.end(), v, 16).ec == std::errc();
      if (!valid) {
        diag(DiagCode::CNT_HLS_0003, "invalid IV `" + text + "` ignored");
        return {};
      }
      bytes[15 - (i / 2)] = static_cast<uint8_t>(bytes[15 - (i / 2)] | (v << ((i % 2) * 4U)));
    }
    return bytes;
  }

  void map_tag(std::string_view value) {
    const auto attrs = attributes("#EXT-X-MAP", value);
    const std::string* uri = find_attr(attrs, "URI");
    if (uri == nullptr || uri->empty()) {
      diag(DiagCode::CNT_HLS_0003, "#EXT-X-MAP without URI ignored");
      return;
    }
    ByteRange range;
    if (const std::string* br = find_attr(attrs, "BYTERANGE")) {
      uint64_t length = 0;
      std::optional<uint64_t> offset;
      if (parse_range(*br, length, offset)) {
        range = {offset.value_or(0), length};
      }
    }
    std::string url = resolve(*uri);
    if (!map_valid || url != map_url || range.offset != map_range.offset || range.length != map_range.length) {
      map_valid = true;
      map_dirty = true;
      map_url = std::move(url);
      map_range = range;
    }
  }

  void stream_inf(const std::vector<Attr>& attrs, bool iframe) {
    Variant v;
    v.is_iframe = iframe;
    v.bandwidth = uint32_attr(attrs, "BANDWIDTH").value_or(0);
    v.average_bandwidth = uint32_attr(attrs, "AVERAGE-BANDWIDTH").value_or(0);
    v.codecs = attr_text(attrs, "CODECS");
    v.supplemental_codecs = attr_text(attrs, "SUPPLEMENTAL-CODECS");
    v.frame_rate = attr_text(attrs, "FRAME-RATE");
    v.video_range = attr_text(attrs, "VIDEO-RANGE");
    v.audio = attr_text(attrs, "AUDIO");
    v.subtitles = attr_text(attrs, "SUBTITLES");
    if (const std::string* r = find_attr(attrs, "RESOLUTION")) {
      const size_t x = r->find('x');
      const auto w = x == std::string::npos ? std::nullopt : parse_uint(std::string_view(*r).substr(0, x));
      const auto h = x == std::string::npos ? std::nullopt : parse_uint(std::string_view(*r).substr(x + 1));
      if (w && h && *w <= std::numeric_limits<uint32_t>::max() && *h <= std::numeric_limits<uint32_t>::max()) {
        v.width = static_cast<uint32_t>(*w);
        v.height = static_cast<uint32_t>(*h);
      } else {
        diag(DiagCode::CNT_HLS_0003, "invalid RESOLUTION `" + *r + "` ignored");
      }
    }
    if (iframe) {
      if (const std::string* uri = find_attr(attrs, "URI"); uri != nullptr && !uri->empty()) {
        v.uri = resolve(*uri);
        m.variants.push_back(std::move(v));
      } else {
        diag(DiagCode::CNT_HLS_0006, "#EXT-X-I-FRAME-STREAM-INF without URI ignored");
      }
      return;
    }
    pending_variant = std::move(v);
    have_stream_inf = true;
  }

  void media_tag(const std::vector<Attr>& attrs) {
    Rendition r;
    r.type = attr_text(attrs, "TYPE");
    r.group_id = attr_text(attrs, "GROUP-ID");
    r.name = attr_text(attrs, "NAME");
    r.language = attr_text(attrs, "LANGUAGE");
    if (const std::string uri = attr_text(attrs, "URI"); !uri.empty()) {
      r.uri = resolve(uri);
    }
    r.is_default = attr_text(attrs, "DEFAULT") == "YES";
    r.autoselect = attr_text(attrs, "AUTOSELECT") == "YES";
    if (r.type.empty() || r.group_id.empty()) {
      diag(DiagCode::CNT_HLS_0003, "#EXT-X-MEDIA without TYPE or GROUP-ID ignored");
      return;
    }
    m.renditions.push_back(std::move(r));
  }

  void clear_segment_state() {
    have_extinf = false;
    extinf_duration.reset();
    have_byterange = false;
    discontinuity = false;
    program_date_time.clear();
  }

  void uri_line(std::string_view line) {
    if (have_stream_inf) {
      pending_variant.uri = resolve(line);
      m.variants.push_back(std::move(pending_variant));
      pending_variant = {};
      have_stream_inf = false;
      return;
    }
    if (!have_extinf) {
      diag(DiagCode::CNT_HLS_0006, "URI line `" + std::string(line) + "` without #EXTINF or #EXT-X-STREAM-INF ignored");
      clear_segment_state();
      return;
    }
    media_segment(resolve(line));
    clear_segment_state();
  }

  void media_segment(std::string url) {
    auto& segs = m.media.segments;
    if (segs.size() + 2 > kMaxSegments) {
      if (!capped) {
        capped = true;
        diag(DiagCode::CNT_HLS_0005, "segment list truncated at " + std::to_string(kMaxSegments) + " entries");
      }
      return;
    }
    if (discontinuity) {
      ++discontinuity_sequence;
    }
    if (map_valid && map_dirty) {
      Segment init;
      init.url = map_url;
      init.byte_range = map_range;
      init.is_init = true;
      init.key_index = current_key;
      init.discontinuity_sequence = discontinuity_sequence;
      segs.push_back(std::move(init));
      map_dirty = false;
    }
    Segment s;
    s.url = std::move(url);
    s.key_index = current_key;
    s.discontinuity_sequence = discontinuity_sequence;
    s.discontinuity = discontinuity;
    s.program_date_time = program_date_time;
    s.duration = extinf_duration.value_or(-1);
    s.start_time = time_us;
    if (s.duration > 0) {
      time_us += s.duration;
      if (m.media.target_duration != 0 && !target_warned &&
          static_cast<uint64_t>((s.duration + 500'000) / 1'000'000) > m.media.target_duration) {
        target_warned = true;
        diag(DiagCode::CNT_HLS_0010,
             "#EXTINF duration rounds above #EXT-X-TARGETDURATION " + std::to_string(m.media.target_duration));
      }
    }
    if (have_byterange) {
      uint64_t offset = 0;
      if (byterange_offset) {
        offset = *byterange_offset;
      } else if (have_last_range && last_range_url == s.url) {
        offset = last_range_end;
      } else {
        diag(DiagCode::CNT_HLS_0004,
             "#EXT-X-BYTERANGE without @offset does not follow a sub-range of `" + s.url + "`; offset 0 assumed");
      }
      if (add_overflows(offset, byterange_length)) {
        diag(DiagCode::CNT_HLS_0003, "byte range beyond 64 bits ignored");
        have_last_range = false;
      } else {
        s.byte_range = {offset, byterange_length};
        have_last_range = true;
        last_range_url = s.url;
        last_range_end = offset + byterange_length;
      }
    } else {
      have_last_range = false;
    }
    segs.push_back(std::move(s));
  }

  // Segment-level tags; false when `name` is not one of them.
  bool segment_tag(std::string_view name, std::string_view value) {
    if (name == "#EXTINF") {
      saw_media_tags = true;
      have_extinf = true;
      const size_t comma = value.find(',');
      extinf_duration = parse_duration_us(value.substr(0, comma));
      if (!extinf_duration) {
        diag(DiagCode::CNT_HLS_0003, "invalid #EXTINF duration `" + std::string(value.substr(0, comma)) + "` ignored");
      }
    } else if (name == "#EXT-X-BYTERANGE") {
      saw_media_tags = true;
      have_byterange = parse_range(value, byterange_length, byterange_offset);
    } else if (name == "#EXT-X-DISCONTINUITY") {
      saw_media_tags = true;
      discontinuity = true;
    } else if (name == "#EXT-X-DISCONTINUITY-SEQUENCE") {
      if (const auto n = parse_uint(value); n && *n <= std::numeric_limits<uint32_t>::max()) {
        m.media.discontinuity_sequence = static_cast<uint32_t>(*n);
        discontinuity_sequence = m.media.discontinuity_sequence;
      } else {
        diag(DiagCode::CNT_HLS_0003, "invalid #EXT-X-DISCONTINUITY-SEQUENCE `" + std::string(value) + "` ignored");
      }
    } else if (name == "#EXT-X-PROGRAM-DATE-TIME") {
      program_date_time = std::string(trim(value));
    } else if (name == "#EXT-X-KEY") {
      saw_media_tags = true;
      current_key = key_tag(name, value, m.media.keys);
    } else if (name == "#EXT-X-MAP") {
      saw_media_tags = true;
      map_tag(value);
    } else {
      return false;
    }
    return true;
  }

  // Master-playlist tags; false when `name` is not one of them.
  bool master_tag(std::string_view name, std::string_view value) {
    if (name == "#EXT-X-STREAM-INF") {
      saw_master_tags = true;
      if (have_stream_inf) {
        diag(DiagCode::CNT_HLS_0006, "#EXT-X-STREAM-INF without a URI line ignored");
      }
      stream_inf(attributes(name, value), false);
    } else if (name == "#EXT-X-I-FRAME-STREAM-INF") {
      saw_master_tags = true;
      stream_inf(attributes(name, value), true);
    } else if (name == "#EXT-X-MEDIA") {
      saw_master_tags = true;
      media_tag(attributes(name, value));
    } else {
      return false;
    }
    return true;
  }

  // Playlist-level tags; unknown tags are ignored (RFC 8216 §4.1).
  void playlist_tag(std::string_view name, std::string_view value) {
    if (name == "#EXT-X-VERSION" || name == "#EXT-X-TARGETDURATION" || name == "#EXT-X-MEDIA-SEQUENCE") {
      const auto n = parse_uint(trim(value));
      if (!n || (*n > std::numeric_limits<uint32_t>::max() && name != "#EXT-X-MEDIA-SEQUENCE")) {
        diag(DiagCode::CNT_HLS_0003, "invalid " + std::string(name) + " `" + std::string(value) + "` ignored");
        return;
      }
      if (name == "#EXT-X-VERSION") {
        m.media.version = static_cast<uint32_t>(*n);
      } else if (name == "#EXT-X-TARGETDURATION") {
        m.media.target_duration = static_cast<uint32_t>(*n);
      } else {
        m.media.media_sequence = *n;
      }
    } else if (name == "#EXT-X-ENDLIST") {
      m.media.end_list = true;
    } else if (name == "#EXT-X-PLAYLIST-TYPE") {
      m.media.playlist_type = std::string(trim(value));
    } else if (name == "#EXT-X-INDEPENDENT-SEGMENTS") {
      m.media.independent_segments = true;
    }
  }

  void tag(std::string_view name, std::string_view value) {
    if (!segment_tag(name, value) && !master_tag(name, value)) {
      playlist_tag(name, value);
    }
  }

  void run(std::string_view text) {
    size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
      const size_t nl = std::min(text.find('\n', pos), text.size());
      line_offset = pos;
      std::string_view line = text.substr(pos, nl - pos);
      pos = nl + 1;
      if (first) {
        first = false;
        if (starts_with(line, "\xEF\xBB\xBF")) {
          line.remove_prefix(3);
        }
        if (trim(line) != "#EXTM3U") {
          diag(DiagCode::CNT_HLS_0001, "first line is not #EXTM3U");
          return;
        }
        m_ok = true;
        continue;
      }
      line = trim(line);
      if (line.empty()) {
        continue;
      }
      if (line.front() != '#') {
        uri_line(line);
      } else if (starts_with(line, "#EXT")) {
        const size_t colon = line.find(':');
        tag(line.substr(0, colon), colon == std::string_view::npos ? std::string_view() : line.substr(colon + 1));
      }
    }
    line_offset = text.size();
    if (have_stream_inf) {
      diag(DiagCode::CNT_HLS_0006, "#EXT-X-STREAM-INF at the end of the playlist has no URI line");
    }
    if (have_extinf) {
      diag(DiagCode::CNT_HLS_0006, "#EXTINF at the end of the playlist has no URI line");
    }
    finish();
  }

  void finish() {
    m.is_master = saw_master_tags;
    if (saw_master_tags && saw_media_tags) {
      diag(DiagCode::CNT_HLS_0009, "playlist mixes master and media tags; treated as a master playlist");
    }
    if (!m.is_master && !m.media.end_list) {
      diag(DiagCode::CNT_HLS_0007, "no #EXT-X-ENDLIST: live playlist snapshotted as written");
    }
    int64_t number =
        static_cast<int64_t>(std::min<uint64_t>(m.media.media_sequence, std::numeric_limits<int64_t>::max() / 2));
    for (Segment& s : m.media.segments) {
      if (!s.is_init) {
        s.number = number++;
      }
    }
  }
};

}  // namespace

ParseResult parse(std::string_view text, const Options& options) {
  Parser p(options);
  p.run(text);
  return {p.m_ok, std::move(p.m)};
}

namespace {

std::string content_type_of(const Variant& v) {
  return v.width == 0 && v.height == 0 && codecs_audio_only(v.codecs) ? "audio" : "video";
}

using RepOffset = flatbuffers::Offset<fb::ManifestRepresentation>;

// One adaptation set under construction: all representations of one media type.
struct SetAcc {
  std::string content_type;
  std::string lang;
  bool lang_seen = false;
  bool lang_mixed = false;
  std::vector<RepOffset> reps;
};

class ManifestWriter {
 public:
  explicit ManifestWriter(flatbuffers::FlatBufferBuilder& fbb) : fbb_(fbb) {}

  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::ManifestEncryption>>> encryption(
      const std::vector<Encryption>& list) {
    std::vector<flatbuffers::Offset<fb::ManifestEncryption>> out;
    out.reserve(list.size());
    for (const Encryption& e : list) {
      out.push_back(fb::CreateManifestEncryption(fbb_, fbb_.CreateString(e.method), fbb_.CreateString(e.key_uri),
                                                 e.iv.empty() ? 0 : fbb_.CreateVector(e.iv)));
    }
    return fbb_.CreateVector(out);
  }

  // A representation with every string and vector present (empty when unknown), so readers never see null.
  RepOffset representation(const std::string& id, const Variant* v,
                           const std::vector<flatbuffers::Offset<fb::ManifestSegment>>& segments,
                           const std::vector<Encryption>& keys) {
    const std::string empty;
    return fb::CreateManifestRepresentation(
        fbb_, fbb_.CreateString(id), v != nullptr ? v->bandwidth : 0,
        fbb_.CreateString(v != nullptr ? v->codecs : empty), fbb_.CreateString(empty), v != nullptr ? v->width : 0,
        v != nullptr ? v->height : 0, fbb_.CreateString(v != nullptr ? v->frame_rate : empty),
        fbb_.CreateString(v != nullptr ? v->video_range : empty), kTimescale, fbb_.CreateVector(segments),
        encryption(keys));
  }

  void add_master(const Manifest& manifest) {
    int64_t video_index = 0;
    int64_t iframe_index = 0;
    for (const Variant& v : manifest.variants) {
      const std::string id = (v.is_iframe ? "i" : "v") + std::to_string(v.is_iframe ? iframe_index++ : video_index++);
      set_for(content_type_of(v)).reps.push_back(representation(id, &v, {}, {}));
    }
    for (const Rendition& r : manifest.renditions) {
      SetAcc& set = set_for(lower(r.type));
      if (!r.language.empty()) {
        set.lang_mixed = set.lang_mixed || (set.lang_seen && set.lang != r.language);
        set.lang = r.language;
        set.lang_seen = true;
      }
      set.reps.push_back(representation(r.group_id + "/" + r.name, nullptr, {}, {}));
    }
  }

  // Returns the period duration in milliseconds.
  int64_t add_media(const MediaPlaylist& media) {
    int64_t total_us = 0;
    std::vector<flatbuffers::Offset<fb::ManifestSegment>> segs;
    segs.reserve(media.segments.size());
    for (const Segment& g : media.segments) {
      const fb::ByteSpan range(g.byte_range.offset, g.byte_range.length);
      segs.push_back(fb::CreateManifestSegment(fbb_, fbb_.CreateString(g.url), &range, g.start_time, g.duration,
                                               g.number, g.is_init));
      total_us += std::max<int64_t>(g.duration, 0);
    }
    set_for("").reps.push_back(representation("0", nullptr, segs, media.keys));
    return total_us / 1000;
  }

  flatbuffers::Offset<fb::ManifestPeriod> period(int64_t duration_ms) {
    std::vector<flatbuffers::Offset<fb::ManifestAdaptationSet>> out;
    out.reserve(sets_.size());
    for (const SetAcc& s : sets_) {
      out.push_back(fb::CreateManifestAdaptationSet(
          fbb_, fbb_.CreateString(std::to_string(out.size())), fbb_.CreateString(s.content_type),
          fbb_.CreateString(s.lang_mixed ? std::string() : s.lang), fbb_.CreateVector(s.reps)));
    }
    return fb::CreateManifestPeriod(fbb_, fbb_.CreateString("0"), 0, duration_ms, fbb_.CreateVector(out));
  }

 private:
  SetAcc& set_for(const std::string& type) {
    for (SetAcc& s : sets_) {
      if (s.content_type == type) {
        return s;
      }
    }
    sets_.push_back({type, {}, false, false, {}});
    return sets_.back();
  }

  flatbuffers::FlatBufferBuilder& fbb_;
  std::vector<SetAcc> sets_;
};

}  // namespace

flatbuffers::Offset<fb::Manifest> write_manifest(flatbuffers::FlatBufferBuilder& fbb, const Manifest& manifest) {
  ManifestWriter writer(fbb);
  int64_t period_duration_ms = -1;
  if (manifest.is_master) {
    writer.add_master(manifest);
  } else {
    period_duration_ms = writer.add_media(manifest.media);
  }
  const auto period = writer.period(period_duration_ms);
  std::vector<flatbuffers::Offset<fb::Diagnostic>> diags;
  diags.reserve(manifest.diagnostics.size());
  for (const Diagnostic& d : manifest.diagnostics) {
    diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
  }
  return fb::CreateManifest(fbb, fbb.CreateString("hls-m3u8"), fbb.CreateString(manifest.url),
                            fbb.CreateVector(&period, 1), writer.encryption(manifest.media.keys),
                            fbb.CreateVector(diags));
}

}  // namespace ovt::container::m3u8
