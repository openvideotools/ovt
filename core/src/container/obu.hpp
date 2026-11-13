// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Raw AV1 OBU streams (chapter 08 §8.6.4, WI-1.227): detection of the two framings (Section 5 low-overhead and
// Annex B length-delimited), the walk that groups OBUs into temporal units, and the re-framing of Annex B OBUs
// into Section 5 form for the plugin ABI (`ovt_stream_unit_bytes`).

#ifndef OVT_CONTAINER_OBU_HPP
#define OVT_CONTAINER_OBU_HPP

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "container/split_types.hpp"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container {

enum class ObuFraming : uint8_t { NotAv1, Section5, AnnexB };

// The ContainerTree `format` field: "section5" / "annexb" ("" for NotAv1).
[[nodiscard]] std::string_view obu_framing_name(ObuFraming f) noexcept;

// obu_type values of AV1 §6.2.2 that §8.6.4 accepts.
inline constexpr uint8_t kObuSequenceHeader = 1;
inline constexpr uint8_t kObuTemporalDelimiter = 2;

// ---- leb128 and obu_header (shared with the unit splitter) ---------------------------------------------------
enum class LebStatus : uint8_t { Ok, Truncated, TooLong };
struct Leb128 {
  LebStatus status = LebStatus::Truncated;
  uint64_t value = 0;
  uint8_t bytes = 0;  // bytes consumed (valid when status == Ok)
};
// AV1 §4.10.5: at most 8 bytes; TooLong when the 8th byte still has its continuation bit. Truncated when `data`
// ends inside the value.
[[nodiscard]] Leb128 read_leb128(std::span<const uint8_t> data) noexcept;
// Appends the shortest leb128 encoding of `value` (< 2^56).
void append_leb128(std::vector<uint8_t>& out, uint64_t value);

struct ObuHeader {
  bool valid = false;  // forbidden bit and reserved bit are 0, obu_type is one §8.6.4 accepts
  uint8_t obu_type = 0;
  bool extension_flag = false;
  bool has_size_field = false;
  uint8_t temporal_id = 0;
  uint8_t spatial_id = 0;
  uint8_t header_bytes = 1;  // 1, or 2 with the extension byte
};
// `data` must hold at least header_bytes bytes (1 or 2); returns valid == false when it is too short.
[[nodiscard]] ObuHeader parse_obu_header(std::span<const uint8_t> data) noexcept;

// ---- detection -------------------------------------------------------------------------------------------------
struct ObuDetectResult {
  ObuFraming framing = ObuFraming::NotAv1;
  bool both_framings = false;       // CNT-OBU-0002: both attempts passed, the first-byte rule chose
  bool missing_size_field = false;  // CNT-OBU-0001: not AV1 only because the first OBU has obu_has_size_field == 0
};
// `prefix` is the first min(file_size, 64 KiB) bytes of the file.
[[nodiscard]] ObuDetectResult obu_detect(std::span<const uint8_t> prefix, uint64_t file_size) noexcept;

// ---- walk ------------------------------------------------------------------------------------------------------
struct ObuNode {
  uint64_t temporal_unit_index = 0;
  int64_t frame_unit_index = -1;  // Annex B only (index within the temporal unit); -1 for Section 5
  FileSpan file_span;             // the OBU: header, obu_size field (when present) and payload
  FileSpan prefix_span;           // Annex B: the obu_length leb128; Section 5: empty
  ObuHeader header;
  uint64_t obu_size = 0;    // payload bytes: the obu_size field, or obu_length - header bytes when absent
  uint64_t obu_length = 0;  // Annex B: the obu_length field value; 0 for Section 5
};

struct FrameUnitNode {  // Annex B only
  uint64_t temporal_unit_index = 0;
  uint64_t frame_unit_index = 0;
  FileSpan file_span;    // the frame unit's content, frame_unit_size bytes
  FileSpan prefix_span;  // the frame_unit_size leb128
  uint64_t obu_count = 0;
};

struct TemporalUnitNode {
  uint64_t index = 0;
  FileSpan
      file_span;  // Section 5: first OBU to the end of the last one; Annex B: the content, temporal_unit_size bytes
  FileSpan prefix_span;  // Annex B: the temporal_unit_size leb128; Section 5: empty
  uint64_t obu_count = 0;
  uint64_t frame_unit_count = 0;  // Annex B only
};

// Called in file order: a temporal unit's OBU and frame unit callbacks precede its own.
struct ObuVisitor {
  std::function<void(const ObuNode&)> on_obu;
  std::function<void(const FrameUnitNode&)> on_frame_unit;
  std::function<void(const TemporalUnitNode&)> on_temporal_unit;
};

struct ObuWalkSummary {
  uint64_t temporal_unit_count = 0;
  uint64_t obu_count = 0;
};

// Walks the whole file in `framing` (Section5 or AnnexB; NotAv1 returns an empty summary). Section 5 groups OBUs
// into temporal units at each OBU_TEMPORAL_DELIMITER (the delimiter is the first OBU of its unit); a stream that
// starts with another OBU puts it in unit 0. Lengths that overrun the file or their enclosing unit are clamped
// (CNT-OBU-0003); a malformed header or leb128 skips the rest of the enclosing unit (Annex B) or ends the walk
// (Section 5) with CNT-OBU-0004; an OBU without obu_has_size_field in a Section 5 stream ends the walk with
// CNT-OBU-0001.
ObuWalkSummary obu_walk(const io::RandomAccessFile& file, ObuFraming framing, const ObuVisitor& visitor,
                        const FindingSink& findings = {});
ObuWalkSummary obu_walk(std::span<const uint8_t> data, uint64_t base_offset, ObuFraming framing,
                        const ObuVisitor& visitor, const FindingSink& findings = {});

// ---- re-framing ------------------------------------------------------------------------------------------------
// Re-frames one Annex B OBU (its file_span bytes, `obu_length` of them) into Section 5 form in `out` (replacing its
// content): obu_has_size_field = 1 and a leb128 obu_size after the header. An OBU that already has the size field
// is copied unchanged. Returns false (and leaves `out` empty) when `obu` is too short for its header or its own
// obu_size field disagrees with its length.
bool reframe_annexb_obu(std::span<const uint8_t> obu, std::vector<uint8_t>& out);

}  // namespace ovt::container

#endif  // OVT_CONTAINER_OBU_HPP
