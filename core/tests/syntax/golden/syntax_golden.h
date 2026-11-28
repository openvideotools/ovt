// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Header-parser golden suite (chapter 14 §14.4.1, §14.4.2, WI-1.572): a whole-stream driver that runs OVT's header
// parsers over the units of a corpus file the way the oracle saw them, and the comparison of the resulting syntax
// trees with the committed oracle goldens.
//
// SPEC-ASSUMPTION(#603): names are compared without CBS's array subscripts; gstparse is compared per member key
// with GStreamer's zero-fill (see gst_compare()).
//
// The driver stands in for `ovt syntax <in> --format json --all-units` (chapter 12 §12.2.3, WI-1.504), which does not
// exist yet; when it does, the CTest entries switch to it and this file keeps only the comparison.

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::test::syntax_golden {

enum class Codec : std::uint8_t { kH264, kH265, kAv1 };

std::optional<Codec> codec_from_name(std::string_view name);

/// One diagnostic of a unit tree, as the chapter 7 Diagnostic carries it.
struct UnitDiagnostic {
  std::string code;
  std::string severity;
  uint64_t bit_offset = 0;
  uint32_t bit_length = 0;
};

/// One unit the oracle decomposes, with OVT's syntax tree of it as an ovt.fb.SyntaxNode FlatBuffer.
struct ParsedUnit {
  uint32_t index = 0;         // the oracle's unit index (units it does not decompose are not counted)
  uint32_t type = 0;          // nal_unit_type / obu_type
  uint64_t bit_length = 0;    // RBSP (NAL header included) or OBU length in bits
  bool parse_error = false;   // a ParseError stopped the unit's parser
  std::vector<uint8_t> rbsp;  // the bytes the tree's bit offsets count in
  std::vector<uint8_t> tree;
  std::vector<UnitDiagnostic> diagnostics;
  /// Values OVT derives or infers that the `gstparse` golden holds but the tree does not carry as coded elements
  /// (an inferred num_ref_idx_l0_active_minus1, the scaling matrices after the fall-back rule, the op counts of the
  /// modification and marking lists): member key (as gst_compare() keys them) -> array of integers.
  std::map<std::string, std::vector<int64_t>> derived;
};

/// The NAL units of an Annex B byte stream (start codes excluded, trailing zero bytes kept as the scanner keeps them).
std::vector<std::span<const uint8_t>> annexb_units(std::span<const uint8_t> stream);

/// The Annex B elementary stream of a container file (ISOBMFF, Matroska): the first video track of the codec, at
/// most `max_packets` packets, converted the way FFmpeg 7.1's `h264_mp4toannexb` / `hevc_mp4toannexb` bitstream
/// filters convert it (H.264: the avcC parameter sets go before the first IDR slice of a packet that carries none;
/// H.265: the hvcC arrays go before the first IRAP unit of every packet). This is the stream
/// `tools/goldens/gen_h26x.py` gives the oracles (`ffmpeg -frames:v 16 -bsf:v <codec>_mp4toannexb`).
std::vector<uint8_t> container_to_annexb(const std::filesystem::path& path, Codec codec, unsigned max_packets);

/// True when the bytes start like an Annex B stream (zero bytes, then 00 00 01).
bool looks_like_annexb(std::span<const uint8_t> head);

/// The NAL units that remain when the stream is cut before the access unit of picture `pictures + 1`, the same cut
/// as `tools/goldens/gen_h26x.py` truncate() makes before it runs the oracles. 0 keeps every unit.
std::vector<std::span<const uint8_t>> cut_to_pictures(std::span<const std::span<const uint8_t>> units, Codec codec,
                                                      unsigned pictures);

/// Parses the units in decode order with OVT's header parsers, keeping the parameter sets as the oracle does (the
/// latest one per id replaces the previous one at once). Units the oracle does not decompose are skipped and do not
/// take an index: H.264 CBS reads nal_unit_type 1, 5, 6, 7, 8, 9, 10, 11, 12, 13 and 19.
std::vector<ParsedUnit> parse_h264(std::span<const std::span<const uint8_t>> units);

/// The same for H.265. CBS reads nal_unit_type 0..9, 16..21, 32..35 and 38..40.
std::vector<ParsedUnit> parse_h265(std::span<const std::span<const uint8_t>> units);

/// One `syntax-elements` record (chapter 14 §14.4.0) with the name reduced to its base (array subscripts dropped).
struct Element {
  uint32_t unit = 0;
  std::string name;
  uint64_t bit_offset = 0;
  uint64_t bit_length = 0;
  nlohmann::json value;
  bool operator==(const Element&) const = default;
};

/// The syntax elements of OVT's trees, in (unit, bit_offset) order: every node with a value except the names in
/// `ignored`. Enum values are the coded integer. A Bytes node whose name is in `byte_arrays` (byte-array elements
/// such as uuid_iso_iec_11578 or user_data_payload_byte, which CBS prints one byte per element) becomes one u(8)
/// element per byte, read from the unit's RBSP; any other Bytes node (the opaque slice data, payload remainders) is
/// left out, as `trace_headers` does not print it.
std::vector<Element> ovt_elements(std::span<const ParsedUnit> units, const std::set<std::string>& ignored,
                                  const std::set<std::string>& byte_arrays);

/// The `trace_headers` golden (a syntax-elements array) with subscripts dropped from the names and the CBS names
/// mapped to spec names by `rename` (tests/oracles/normalize/cbs_name_map.json, `rename.<codec>`).
std::vector<Element> golden_elements(const nlohmann::json& golden, const std::map<std::string, std::string>& rename);

/// Element-by-element comparison: every field equal, same count. Returns one line per difference (at most
/// `max_lines`, then a count of the rest); empty when equal.
std::vector<std::string> compare_elements(const std::vector<Element>& golden, const std::vector<Element>& ovt,
                                          size_t max_lines = 40);

/// The test-side map between the `gstparse` golden and OVT's trees (core/tests/syntax/golden/gst_map.json, one
/// object per codec).
struct GstMap {
  std::set<std::string> structs;                            // golden keys that hold a parsed structure (sps, pps, ...)
  std::map<std::string, std::vector<uint32_t>> unit_types;  // structure -> the unit types that carry it
  std::set<std::string> qualified_parents;                  // golden members whose name prefixes their leaves' keys
  std::map<std::string, std::string> rename;                // "<structure>.<key>" -> OVT key
  std::map<std::string, std::string> skip;                  // "<structure>.<key>" -> why it is not compared
};

GstMap read_gst_map(const std::filesystem::path& path, Codec codec);

/// `gstparse` comparison (chapter 14 §14.4.2): the golden's units that hold a structure are matched in order with
/// OVT's units of the same types. In each, every golden member is reduced to a key (its name, or
/// "<parent>.<name>" for a qualified parent) and the integer values of all its occurrences in document order;
/// OVT's tree gives the same per key (a struct that follows a sibling "<X>_present_flag" is qualified as X),
/// overridden by ParsedUnit::derived. A key OVT has must hold equal values; a key OVT does not have must hold only
/// zeros (GStreamer zero-fills what is not coded) unless the map skips it. Returns one line per difference.
std::vector<std::string> gst_compare(const nlohmann::json& golden, std::span<const ParsedUnit> units, const GstMap& map,
                                     size_t max_lines = 40);

/// Text of a file, gunzipped when the name ends in ".gz".
std::string read_text(const std::filesystem::path& path);

/// The non-comment, non-blank lines of a list file (trace_headers_ignore.txt).
std::set<std::string> read_list(const std::filesystem::path& path);

/// `rename.<codec>` of cbs_name_map.json.
std::map<std::string, std::string> read_rename(const std::filesystem::path& path, Codec codec);

/// The spec names of every element CBS can print for the codec: `known.<codec>` of cbs_name_map.json, each mapped
/// through `rename.<codec>`.
std::set<std::string> read_known(const std::filesystem::path& path, Codec codec);

}  // namespace ovt::test::syntax_golden
