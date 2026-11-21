// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Header-parser golden suite: whole-stream driver and comparison (chapter 14 §14.4.1, §14.4.2, WI-1.572).

#include "syntax_golden.h"

#include "h264_derived.h"
#include "h265_derived.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "demux/demuxer.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/aud.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sei.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h264/sps_ext.hpp"
#include "ovt/cxx/syntax/h265/aud.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt::test::syntax_golden {

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
namespace h264 = ovt::syntax::h264;
namespace h265 = ovt::syntax::h265;

constexpr std::string_view kOpaquePrefix = "opaque:";

constexpr std::array<std::string_view, 4> kSeverity = {"Info", "Warning", "Error", "Fatal"};

void collect_diagnostics(const SyntaxNodeData& node, std::vector<UnitDiagnostic>& out) {
  for (const auto* d = node.first_diag; d != nullptr; d = d->next) {
    out.push_back({std::string(d->code), std::string(kSeverity.at(static_cast<size_t>(d->severity) & 3U)),
                   d->span.bit_offset, d->span.bit_length});
  }
  for (const auto* c = node.first_child; c != nullptr; c = c->next_sibling) {
    collect_diagnostics(*c, out);
  }
}

void finish_unit(SyntaxBuilder& b, ParsedUnit& unit) {
  SyntaxNodeBuffer tree = b.finish();
  if (tree.empty()) {
    return;
  }
  tree.set_root_name("nal_unit");
  collect_diagnostics(*tree.root(), unit.diagnostics);
  unit.tree = tree.to_flatbuffer();
}

uint32_t h26x_type(std::span<const uint8_t> unit, Codec codec) {
  if (unit.empty()) {
    return 0xFF;
  }
  return codec == Codec::kH264 ? (unit[0] & 0x1FU) : ((unit[0] >> 1U) & 0x3FU);
}

// True for the first slice of a picture (gen_h26x.py first_slice_of_picture()).
bool first_slice_of_picture(std::span<const uint8_t> unit, Codec codec) {
  const uint32_t t = h26x_type(unit, codec);
  if (codec == Codec::kH264) {
    return t >= 1 && t <= 5 && unit.size() > 1 && (unit[1] & 0x80U) != 0;  // first_mb_in_slice == 0: ue(v) '1'
  }
  return t <= 21 && (t < 10 || t > 15) && unit.size() > 2 && (unit[2] & 0x80U) != 0;
}

bool au_prefix(uint32_t t, Codec codec) {
  if (codec == Codec::kH264) {
    return t == 6 || t == 7 || t == 8 || t == 9 || t == 14 || t == 15;
  }
  return t == 32 || t == 33 || t == 34 || t == 35 || t == 39 || (t >= 41 && t <= 44) || (t >= 48 && t <= 55);
}

// The H.265 NAL unit types FFmpeg's CBS decomposes (cbs_h265_read_nal_unit): slice segments, VPS, SPS, PPS, AUD,
// FD and SEI. The others are not counted in its unit indices.
bool h265_decomposed(uint32_t t) {
  return t <= 9 || (t >= 16 && t <= 21) || (t >= 32 && t <= 35) || (t >= 38 && t <= 40);
}

// Base name of a CBS element: "chroma_weight_l0[3][1]" -> "chroma_weight_l0".
std::string base_name(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  int depth = 0;
  for (const char c : name) {
    if (c == '[') {
      ++depth;
    } else if (c == ']') {
      depth = std::max(0, depth - 1);
    } else if (depth == 0) {
      out.push_back(c);
    }
  }
  return out;
}

nlohmann::json node_value(const fb::SyntaxNode& n) {
  switch (n.kind()) {
    case fb::SyntaxValueKind::UInt:
    case fb::SyntaxValueKind::Enum:
      return n.uvalue();
    case fb::SyntaxValueKind::Int:
    case fb::SyntaxValueKind::Fixed:
      return n.ivalue();
    case fb::SyntaxValueKind::Bool:
      return n.bvalue() ? 1 : 0;
    case fb::SyntaxValueKind::String:
      return n.svalue() != nullptr ? n.svalue()->str() : std::string();
    default:
      return nullptr;
  }
}

struct Collect;
bool ignored_name(const Collect& c, const std::string& name);

struct Collect {
  const ParsedUnit& unit;
  const std::set<std::string>& ignored;
  const std::set<std::string>& byte_arrays;
  std::vector<Element>& out;
};

bool ignored_name(const Collect& c, const std::string& name) {
  return c.ignored.contains(name);
}

// Parameter-set extensions OVT keeps opaque in phase 1 (chapter 7 §7.3.5 rows 2 and 4) that CBS reads as a run of
// one-bit `extension_data` elements up to the rbsp_stop_one_bit (H.265 vps/sps/pps_extension_data_flag).
// SPEC-ASSUMPTION(#603): compared bit by bit, so OVT's opaque span must cover exactly the bits CBS reads.
const std::set<std::string, std::less<>> kOpaqueExtensions = {"vps_extension", "sps_extension_data_flag",
                                                              "pps_extension_data_flag"};

void push_extension_bits(const Collect& c, uint64_t begin, uint64_t end) {
  const auto& rbsp = c.unit.rbsp;
  // The rbsp_stop_one_bit is the last 1 bit of the RBSP; the extension bits end before it.
  uint64_t stop = 0;
  for (size_t i = rbsp.size(); i-- > 0;) {
    if (rbsp[i] != 0) {
      stop = (i * 8U) + 7U - static_cast<uint64_t>(std::countr_zero(rbsp[i]));
      break;
    }
  }
  for (uint64_t bit = begin; bit < std::min(end, stop); ++bit) {
    const unsigned v = (rbsp.at(bit / 8) >> (7U - (bit % 8))) & 1U;
    c.out.push_back({c.unit.index, "extension_data", bit, 1, v});
  }
}

// Extensions §7.3.5 keeps opaque in phase 1 that CBS parses element by element. SPEC-ASSUMPTION(#663): compared for
// coverage only; OVT's opaque node must start at the first golden element of the extension and hold every golden
// element that follows in the unit. The marker element carries kOpaquePrefix + the node name.
const std::set<std::string, std::less<>> kCoverageOnly = {"pps_multilayer_extension", "pps_3d_extension",
                                                          "sps_3d_extension"};

// T.35 payload fields OVT reads from the leading itu_t_t35_payload_byte()s (WI-1.120); CBS prints the bytes.
const std::set<std::string, std::less<>> kT35PayloadFields = {"itu_t_t35_terminal_provider_code",
                                                              "itu_t_t35_terminal_provider_oriented_code"};

// The node names of SEI payloads OVT keeps as bytes (no parser, or a parameter set missing): the Annex D names.
// CBS prints the bytes of a payload it does not parse as payload_byte.
const std::set<std::string, std::less<>>& sei_payload_names() {
  static const std::set<std::string, std::less<>> names = [] {
    std::set<std::string, std::less<>> out;
    for (uint32_t t = 0; t <= 205; ++t) {
      out.emplace(h265::sei_payload_name(t));
    }
    return out;
  }();
  return names;
}

// One u(8) element per RBSP byte of a byte-aligned span, named as CBS names the bytes of a payload it does not parse.
void push_bytes(const Collect& c, const std::string& name, uint64_t offset, uint64_t length) {
  if (offset % 8 != 0 || length % 8 != 0 || (offset + length) / 8 > c.unit.rbsp.size()) {
    return;
  }
  for (uint64_t bit = offset; bit < offset + length; bit += 8) {
    c.out.push_back({c.unit.index, name, bit, 8, c.unit.rbsp.at(bit / 8)});
  }
}

void collect_elements(const fb::SyntaxNode& n, const Collect& c) {
  const auto kind = n.kind();
  const std::string name = n.name() != nullptr ? n.name()->str() : std::string();
  const fb::BitSpan* span = n.span();
  const uint64_t offset = span != nullptr ? span->bit_offset() : 0U;
  const uint64_t length = span != nullptr ? span->bit_length() : 0U;
  if (kind == fb::SyntaxValueKind::Bytes) {
    // Byte-aligned byte arrays only; the bytes come from the RBSP (the tree keeps a copy only up to 256 bytes).
    if (c.byte_arrays.contains(name)) {
      push_bytes(c, name, offset, length);
    } else if (kOpaqueExtensions.contains(name)) {
      push_extension_bits(c, offset, offset + length);
    } else if (kCoverageOnly.contains(name)) {
      c.out.push_back({c.unit.index, std::string(kOpaquePrefix) + name, offset, length, nullptr});
    } else if (sei_payload_names().contains(name)) {
      push_bytes(c, "payload_byte", offset, length);
    }
  } else if (kT35PayloadFields.contains(name)) {
    push_bytes(c, "itu_t_t35_payload_byte", offset, length);
  } else if (kind != fb::SyntaxValueKind::None && !ignored_name(c, name)) {
    c.out.push_back({c.unit.index, name, offset, length, node_value(n)});
  }
  if (n.children() != nullptr) {
    for (const fb::SyntaxNode* child : *n.children()) {
      collect_elements(*child, c);
    }
  }
}

// CBS reads an element wider than 32 bits in pieces (general_reserved_zero_43bits as 24 + 19 bits) and prints each
// piece under the element's name. `next` continues `head` when it is the next piece of the same read: same name and
// unit, adjacent, and the width the name states ("_<N>bits", N > 32) not reached yet.
bool continues_split_read(const Element& head, const Element& next) {
  static const std::regex kWidth("_([0-9]+)bits$");
  std::smatch m;
  if (head.unit != next.unit || head.name != next.name || head.bit_offset + head.bit_length != next.bit_offset ||
      !std::regex_search(head.name, m, kWidth)) {
    return false;
  }
  const uint64_t width = std::stoull(m[1].str());
  return width > 32 && head.bit_length + next.bit_length <= width && head.value.is_number_unsigned() &&
         next.value.is_number_unsigned();
}

void sort_elements(std::vector<Element>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Element& a, const Element& b) {
    return a.unit != b.unit ? a.unit < b.unit : a.bit_offset < b.bit_offset;
  });
}

std::string describe(const Element& e) {
  return fmt::format("{} [{}+{}] = {}", e.name, e.bit_offset, e.bit_length, e.value.dump());
}

// h264_mp4toannexb_filter() of FFmpeg 7.1: the avcC parameter sets go before the first IDR slice of a packet that
// carries none; new_idr persists across packets, sps_seen / pps_seen are per packet.
class H264Mp4ToAnnexB {
 public:
  H264Mp4ToAnnexB(std::span<const uint8_t> avcc, unsigned length_size) : length_size_(length_size) {
    // avcC (ISO/IEC 14496-15 §5.3.3.1): the SPS list, then the PPS list, each entry with a 16-bit length.
    if (avcc.size() < 7) {
      throw std::runtime_error("short avcC");
    }
    size_t pos = 5;
    const unsigned sps_count = avcc[pos++] & 0x1FU;
    copy_list(avcc, pos, sps_count, sps_);
    if (pos < avcc.size()) {
      const unsigned count = avcc[pos++];
      copy_list(avcc, pos, count, pps_);
    }
  }

  void packet(std::span<const uint8_t> data, std::vector<uint8_t>& es) {
    sps_seen_ = false;
    pps_seen_ = false;
    size_t p = 0;
    while (p + length_size_ <= data.size()) {
      size_t len = 0;
      for (unsigned k = 0; k < length_size_; ++k) {
        len = (len << 8U) | data[p + k];
      }
      p += length_size_;
      if (len == 0 || p + len > data.size()) {
        break;
      }
      unit(data.subspan(p, len), es);
      p += len;
    }
  }

 private:
  static void copy_list(std::span<const uint8_t> avcc, size_t& pos, unsigned count, std::vector<uint8_t>& out) {
    for (unsigned k = 0; k < count && pos + 2 <= avcc.size(); ++k) {
      const size_t len = (size_t{avcc[pos]} << 8U) | avcc[pos + 1];
      pos += 2;
      if (pos + len > avcc.size()) {
        throw std::runtime_error("truncated avcC");
      }
      append_unit(avcc.subspan(pos, len), out);
      pos += len;
    }
  }

  static void append_unit(std::span<const uint8_t> nal, std::vector<uint8_t>& out) {
    // FFmpeg writes 4-byte start codes for parameter sets and the first unit of a packet, 3-byte ones otherwise;
    // the unit scanner treats both alike.
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), nal.begin(), nal.end());
  }

  void unit(std::span<const uint8_t> nal, std::vector<uint8_t>& es) {
    const unsigned type = nal[0] & 0x1FU;
    if (type == 7) {
      sps_seen_ = new_idr_ = true;
    } else if (type == 8) {
      pps_seen_ = new_idr_ = true;
      if (!sps_seen_ && !sps_.empty()) {
        es.insert(es.end(), sps_.begin(), sps_.end());
        sps_seen_ = true;
      }
    }
    const bool idr = type == 5;
    if (!new_idr_ && idr && nal.size() > 1 && (nal[1] & 0x80U) != 0) {
      new_idr_ = true;
    }
    if (new_idr_ && idr && !sps_seen_ && !pps_seen_) {
      es.insert(es.end(), sps_.begin(), sps_.end());
      es.insert(es.end(), pps_.begin(), pps_.end());
      new_idr_ = false;
    } else if (new_idr_ && idr && sps_seen_ && !pps_seen_) {
      es.insert(es.end(), pps_.begin(), pps_.end());
    }
    append_unit(nal, es);
    if (type == 1) {
      new_idr_ = true;
      sps_seen_ = false;
      pps_seen_ = false;
    }
  }

  unsigned length_size_;
  std::vector<uint8_t> sps_;
  std::vector<uint8_t> pps_;
  bool new_idr_ = true;
  bool sps_seen_ = false;
  bool pps_seen_ = false;
};

// hevc_mp4toannexb_filter() of FFmpeg 7.1: the hvcC arrays (all of them, in order) become the extradata, which goes
// before the first IRAP NAL unit (types 16..23) of every packet.
class HevcMp4ToAnnexB {
 public:
  HevcMp4ToAnnexB(std::span<const uint8_t> hvcc, unsigned length_size) : length_size_(length_size) {
    // hvcC (ISO/IEC 14496-15 §8.3.3.1): 22 bytes of header, numOfArrays, then per array one byte of type and a
    // 16-bit numNalus, each NAL unit with a 16-bit length.
    if (hvcc.size() < 23) {
      throw std::runtime_error("short hvcC");
    }
    size_t pos = 22;
    const unsigned arrays = hvcc[pos++];
    for (unsigned a = 0; a < arrays && pos + 3 <= hvcc.size(); ++a) {
      const unsigned count = (unsigned{hvcc[pos + 1]} << 8U) | hvcc[pos + 2];
      pos += 3;
      for (unsigned k = 0; k < count && pos + 2 <= hvcc.size(); ++k) {
        const size_t len = (size_t{hvcc[pos]} << 8U) | hvcc[pos + 1];
        pos += 2;
        if (pos + len > hvcc.size()) {
          throw std::runtime_error("truncated hvcC");
        }
        append(hvcc.subspan(pos, len), extradata_);
        pos += len;
      }
    }
  }

  void packet(std::span<const uint8_t> data, std::vector<uint8_t>& es) const {
    bool got_irap = false;
    size_t p = 0;
    while (p + length_size_ <= data.size()) {
      size_t len = 0;
      for (unsigned k = 0; k < length_size_; ++k) {
        len = (len << 8U) | data[p + k];
      }
      p += length_size_;
      if (len == 0 || p + len > data.size()) {
        break;
      }
      const unsigned type = (data[p] >> 1U) & 0x3FU;
      const bool irap = type >= 16 && type <= 23;
      if (irap && !got_irap) {
        es.insert(es.end(), extradata_.begin(), extradata_.end());
      }
      got_irap = got_irap || irap;
      append(data.subspan(p, len), es);
      p += len;
    }
  }

 private:
  static void append(std::span<const uint8_t> nal, std::vector<uint8_t>& out) {
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), nal.begin(), nal.end());
  }

  unsigned length_size_;
  std::vector<uint8_t> extradata_;
};

using Values = std::map<std::string, std::vector<int64_t>>;

// "<qualifier>.<name>", or the name alone without a qualifier.
std::string qualified(const std::string& qualifier, const std::string& name) {
  if (qualifier.empty() || qualifier == name) {
    return name;
  }
  std::string key = qualifier;
  key += '.';
  key += name;
  return key;
}

void flatten_golden(const nlohmann::json& v, const std::string& key, const std::string& qualifier, const GstMap& map,
                    Values& out) {
  if (v.is_object()) {
    for (const auto& [k, child] : v.items()) {
      flatten_golden(child, k, map.qualified_parents.contains(k) ? k : qualifier, map, out);
    }
  } else if (v.is_array()) {
    (void)out[qualified(qualifier, key)];  // an empty array is kept as a key without values
    for (const auto& child : v) {
      flatten_golden(child, key, qualifier, map, out);
    }
  } else if (v.is_number_integer() || v.is_boolean()) {
    out[qualified(qualifier, key)].push_back(v.is_boolean() ? (v.get<bool>() ? 1 : 0) : v.get<int64_t>());
  }
}

void flatten_tree(const fb::SyntaxNode& n, const std::string& qualifier, const GstMap& map, Values& out) {
  if (n.children() == nullptr) {
    return;
  }
  const fb::SyntaxNode* prev = nullptr;
  for (const fb::SyntaxNode* c : *n.children()) {
    const std::string name = c->name() != nullptr ? c->name()->str() : std::string();
    const auto kind = c->kind();
    if (kind != fb::SyntaxValueKind::None && kind != fb::SyntaxValueKind::Bytes &&
        kind != fb::SyntaxValueKind::String) {
      out[qualified(qualifier, name)].push_back(node_value(*c).get<int64_t>());
    }
    std::string q = qualifier;
    if (kind == fb::SyntaxValueKind::None && prev != nullptr && prev->name() != nullptr) {
      const std::string p = prev->name()->str();
      constexpr std::string_view kFlag = "_present_flag";
      if (p.ends_with(kFlag)) {
        const std::string parent = p.substr(0, p.size() - kFlag.size());
        if (map.qualified_parents.contains(parent)) {
          q = parent;
        }
      }
    }
    flatten_tree(*c, q, map, out);
    prev = c;
  }
}

std::string join(const std::vector<int64_t>& v) {
  constexpr size_t kShown = 24;
  std::string s = "[";
  for (size_t i = 0; i < v.size() && i < kShown; ++i) {
    s += (i == 0 ? "" : ",") + std::to_string(v[i]);
  }
  return s + (v.size() > kShown ? fmt::format(",... ({} values)]", v.size()) : "]");
}

// The key of the structure a golden unit holds ("sps", ...), or "" for a unit GStreamer did not parse.
std::string golden_structure(const nlohmann::json& unit, const GstMap& map) {
  for (const auto& [k, v] : unit.items()) {
    if (map.structs.contains(k)) {
      return k;
    }
  }
  return {};
}

// True when `golden` is `ovt` followed by zeros only: GStreamer's fixed-size arrays hold the coded entries first and
// zero-fill the rest (SPEC-ASSUMPTION(#603), the zero-fill rule of gst_compare()).
bool zero_extended(const std::vector<int64_t>& ovt, const std::vector<int64_t>& golden) {
  return golden.size() >= ovt.size() && std::equal(ovt.begin(), ovt.end(), golden.begin()) &&
         std::all_of(golden.begin() + static_cast<std::ptrdiff_t>(ovt.size()), golden.end(),
                     [](int64_t x) { return x == 0; });
}

template <class Note>
void compare_unit(const std::string& unit_key, const std::string& structure, const nlohmann::json& golden,
                  const ParsedUnit& u, const GstMap& map, const Note& note) {
  const auto& types = map.unit_types.at(structure);
  if (std::find(types.begin(), types.end(), u.type) == types.end()) {
    note(fmt::format("golden unit {} ({}) matched OVT unit {} of type {}", unit_key, structure, u.index, u.type));
    return;
  }
  Values gold;
  flatten_golden(golden, structure, std::string(), map, gold);
  Values ovt;
  if (!u.tree.empty()) {
    flatten_tree(*flatbuffers::GetRoot<fb::SyntaxNode>(u.tree.data()), std::string(), map, ovt);
  }
  for (const auto& [k, v] : u.derived) {
    ovt[k] = v;
  }
  for (const auto& [raw_key, values] : gold) {
    const std::string member = qualified(structure, raw_key);
    if (map.skip.contains(member)) {
      continue;
    }
    const auto renamed = map.rename.find(member);
    const std::string key = renamed != map.rename.end() ? renamed->second : raw_key;
    const auto it = ovt.find(key);
    if (it == ovt.end()) {
      if (std::any_of(values.begin(), values.end(), [](int64_t x) { return x != 0; })) {
        note(
            fmt::format("golden unit {} {}: {} = {} | OVT has no {}", unit_key, structure, raw_key, join(values), key));
      }
    } else if (!zero_extended(it->second, values)) {
      note(fmt::format("golden unit {} {}: {} = {} | OVT {} = {}", unit_key, structure, raw_key, join(values), key,
                       join(it->second)));
    }
  }
}

}  // namespace

std::optional<Codec> codec_from_name(std::string_view name) {
  if (name == "h264") {
    return Codec::kH264;
  }
  if (name == "h265") {
    return Codec::kH265;
  }
  if (name == "av1") {
    return Codec::kAv1;
  }
  return std::nullopt;
}

std::vector<std::span<const uint8_t>> annexb_units(std::span<const uint8_t> stream) {
  std::vector<std::span<const uint8_t>> out;
  ovt::syntax::scan_annexb(stream, 0, ovt::syntax::kNoContainerSample,
                           [&](const ovt::syntax::ScannedUnit& u) { out.push_back(u.bytes); });
  return out;
}

bool looks_like_annexb(std::span<const uint8_t> head) {
  size_t i = 0;
  while (i < head.size() && i < 8 && head[i] == 0) {
    ++i;
  }
  return i >= 2 && i < head.size() && head[i] == 1;
}

std::vector<uint8_t> container_to_annexb(const std::filesystem::path& path, Codec codec, unsigned max_packets) {
  std::shared_ptr<ovt::io::RandomAccessFile> file = ovt::io::RandomAccessFile::open(path.string());
  auto opened = ovt::demux::Demuxer::open(file, "");
  if (!opened) {
    throw std::runtime_error("cannot open " + path.string() + ": " + opened.message());
  }
  auto& demuxer = *opened.value();
  const bool avc = codec == Codec::kH264;
  const auto fb_codec = avc ? ovt::fb::Codec::H264 : ovt::fb::Codec::H265;
  const auto& streams = demuxer.streams();
  const auto video = std::find_if(streams.begin(), streams.end(), [&](const ovt::demux::StreamInfo& s) {
    return s.codec == fb_codec && s.media_type == AVMEDIA_TYPE_VIDEO;
  });
  const std::string record = avc ? "avcC" : "hvcC";
  if (video == streams.end() || !video->length_prefixed || video->codec_private_fourcc != record) {
    throw std::runtime_error("no length-prefixed video track with an " + record + " record in " + path.string());
  }
  const int stream = video->avstream_index;
  std::optional<H264Mp4ToAnnexB> h264_filter;
  std::optional<HevcMp4ToAnnexB> hevc_filter;
  if (avc) {
    h264_filter.emplace(video->codec_private, video->nal_length_size);
  } else {
    hevc_filter.emplace(video->codec_private, video->nal_length_size);
  }
  demuxer.select_streams(std::span<const int>(&stream, 1));
  std::vector<uint8_t> es;
  for (unsigned n = 0; n < max_packets; ++n) {
    auto r = demuxer.read_packet();
    if (!r) {
      throw std::runtime_error("read_packet: " + r.message());
    }
    if (!r.value()) {
      break;
    }
    if (avc) {
      h264_filter->packet(r.value()->data, es);
    } else {
      hevc_filter->packet(r.value()->data, es);
    }
  }
  return es;
}

std::vector<std::span<const uint8_t>> cut_to_pictures(std::span<const std::span<const uint8_t>> units, Codec codec,
                                                      unsigned pictures) {
  std::vector<std::span<const uint8_t>> out(units.begin(), units.end());
  if (pictures == 0) {
    return out;
  }
  unsigned count = 0;
  for (size_t i = 0; i < units.size(); ++i) {
    if (!first_slice_of_picture(units[i], codec)) {
      continue;
    }
    if (++count == pictures + 1) {
      size_t j = i;
      while (j > 0 && au_prefix(h26x_type(units[j - 1], codec), codec)) {
        --j;
      }
      out.resize(j);
      break;
    }
  }
  return out;
}

std::vector<ParsedUnit> parse_h264(std::span<const std::span<const uint8_t>> units) {
  std::vector<ParsedUnit> out;
  // The latest SPS / PPS per id, as CBS keeps them. unique_ptr keeps a replaced set alive for the tables' pointers
  // until the slot is overwritten; the tables are rebuilt after every put.
  std::array<std::unique_ptr<h264::Sps>, 32> sps{};
  std::array<std::unique_ptr<h264::Pps>, 256> pps{};
  h264::ParameterSets sets;
  std::array<const h264::Sps*, 32> sps_by_id{};
  h264::SeiContext sei_ctx;
  sei_ctx.sps_by_id = sps_by_id;
  uint32_t index = 0;

  for (const auto nal : units) {
    const uint32_t type = h26x_type(nal, Codec::kH264);
    const bool decomposed = (type >= 1 && type <= 13 && type != 2 && type != 3 && type != 4) || type == 19;
    if (!decomposed) {
      continue;
    }
    ParsedUnit unit;
    unit.index = index++;
    unit.type = type;
    unit.rbsp = ovt::rbsp_from_nal(nal).rbsp;
    unit.bit_length = unit.rbsp.size() * 8U;
    BitReader br(unit.rbsp);
    SyntaxBuilder b(br);
    try {
      const h264::NalHeader h = h264::parse_nal_header(br, b);
      switch (type) {
        case 1:
        case 5:
        case 19: {
          const auto sh = h264::parse_slice_layer(br, b, h, sets);
          if (sh) {
            const h264::Pps* p = sets.pps.find(sh->pic_parameter_set_id);
            sei_ctx.active_sps = p != nullptr ? sets.sps.find(p->seq_parameter_set_id) : nullptr;
            add_h264_slice_derived(*sh, sei_ctx.active_sps, unit);
          }
          sei_ctx.buffering_period_sps_id.reset();
          break;
        }
        case 6:
          (void)h264::parse_sei_rbsp(br, b, sei_ctx);
          break;
        case 7: {
          auto s = std::make_unique<h264::Sps>(h264::parse_sps(br, b));
          add_h264_sps_derived(*s, unit);
          const uint32_t id = s->seq_parameter_set_id;
          if (id < sps.size()) {
            sps.at(id) = std::move(s);
            sets.sps.entries.at(id) = sps.at(id).get();
            sps_by_id.at(id) = sps.at(id).get();
          }
          break;
        }
        case 8: {
          auto p = std::make_unique<h264::Pps>(h264::parse_pps(br, b, sets.sps));
          add_h264_pps_derived(*p, sets.sps.find(p->seq_parameter_set_id), unit);
          const uint32_t id = p->pic_parameter_set_id;
          if (id < pps.size()) {
            pps.at(id) = std::move(p);
            sets.pps.entries.at(id) = pps.at(id).get();
          }
          break;
        }
        case 9:
          (void)h264::parse_access_unit_delimiter(br, b);
          break;
        case 10:
          (void)h264::parse_end_of_seq(br, b);
          break;
        case 11:
          (void)h264::parse_end_of_stream(br, b);
          break;
        case 12:
          (void)h264::parse_filler_data(br, b);
          break;
        default:  // 13
          (void)h264::parse_sps_extension(br, b);
          break;
      }
    } catch (const ovt::ParseError& e) {
      unit.parse_error = true;
      unit.diagnostics.push_back({e.code, "Error", e.bit_offset, 0});
    }
    finish_unit(b, unit);
    out.push_back(std::move(unit));
  }
  return out;
}

// The parameter sets of an H.265 stream as CBS keeps them: the latest one per id replaces the previous one at once.
// The PPS table parse_slice_segment_header() takes is rebuilt after every put, activating each PPS against the
// SPS it names.
class H265Sets {
 public:
  void put(std::unique_ptr<h265::Vps> v) {
    const uint32_t id = v->vps_video_parameter_set_id;
    vps_.at(id) = std::move(v);
    vps_by_id_.at(id) = vps_.at(id).get();
  }
  void put(std::unique_ptr<h265::Sps> s) {
    const uint32_t id = s->sps_seq_parameter_set_id;
    if (id < sps_.size()) {
      sps_.at(id) = std::move(s);
      sps_by_id_.at(id) = sps_.at(id).get();
      rebuild();
    }
  }
  void put(std::unique_ptr<h265::Pps> p) {
    const uint32_t id = p->pps_pic_parameter_set_id;
    if (id < pps_.size()) {
      pps_.at(id) = std::move(p);
      rebuild();
    }
  }
  [[nodiscard]] const h265::PpsTable& table() const { return table_; }
  [[nodiscard]] const h265::Sps* sps(uint32_t id) const { return id < sps_.size() ? sps_.at(id).get() : nullptr; }
  [[nodiscard]] std::span<const h265::Vps* const> vps_by_id() const { return vps_by_id_; }
  [[nodiscard]] std::span<const h265::Sps* const> sps_by_id() const { return sps_by_id_; }

 private:
  void rebuild() {
    for (size_t id = 0; id < pps_.size(); ++id) {
      const h265::Pps* p = pps_.at(id).get();
      const h265::Sps* s = p != nullptr && p->pps_seq_parameter_set_id < sps_.size()
                               ? sps_.at(p->pps_seq_parameter_set_id).get()
                               : nullptr;
      active_.at(id) = s != nullptr ? std::make_unique<h265::ActivePps>(h265::activate_pps(*p, *s)) : nullptr;
      table_.entries.at(id) = active_.at(id).get();
    }
  }

  std::array<std::unique_ptr<h265::Vps>, 16> vps_{};
  std::array<std::unique_ptr<h265::Sps>, 16> sps_{};
  std::array<std::unique_ptr<h265::Pps>, 64> pps_{};
  std::array<std::unique_ptr<h265::ActivePps>, 64> active_{};
  std::array<const h265::Vps*, 16> vps_by_id_{};
  std::array<const h265::Sps*, 16> sps_by_id_{};
  h265::PpsTable table_{};
};

std::vector<ParsedUnit> parse_h265(std::span<const std::span<const uint8_t>> units) {
  std::vector<ParsedUnit> out;
  H265Sets sets;
  h265::SeiContext sei_ctx;
  sei_ctx.vps_by_id = sets.vps_by_id();
  sei_ctx.sps_by_id = sets.sps_by_id();
  std::optional<h265::SliceSegmentHeader> prev_independent;
  uint32_t index = 0;

  for (const auto nal : units) {
    const uint32_t type = h26x_type(nal, Codec::kH265);
    if (!h265_decomposed(type)) {
      continue;
    }
    ParsedUnit unit;
    unit.index = index++;
    unit.type = type;
    unit.rbsp = ovt::rbsp_from_nal(nal).rbsp;
    unit.bit_length = unit.rbsp.size() * 8U;
    BitReader br(unit.rbsp);
    SyntaxBuilder b(br);
    try {
      const h265::NalHeader h = h265::parse_nal_header(br, b);
      if (type <= 21) {
        auto sh =
            h265::parse_slice_segment_header(br, b, h, sets.table(), prev_independent ? &*prev_independent : nullptr);
        const h265::ActivePps* a = sets.table().entries.at(sh.slice_pic_parameter_set_id);
        sei_ctx.active_sps = a != nullptr ? a->sps : nullptr;
        sei_ctx.buffering_period_sps_id.reset();
        add_h265_slice_derived(sh, a, unit);
        if (!sh.dependent_slice_segment_flag) {
          prev_independent = std::move(sh);
        }
      } else if (type == 32) {
        auto v = std::make_unique<h265::Vps>(h265::parse_vps(br, b));
        add_h265_vps_derived(*v, unit);
        sets.put(std::move(v));
      } else if (type == 33) {
        auto s = std::make_unique<h265::Sps>(h265::parse_sps(br, b, h.nuh_layer_id));
        add_h265_sps_derived(*s, unit);
        sets.put(std::move(s));
      } else if (type == 34) {
        auto p = std::make_unique<h265::Pps>(h265::parse_pps(br, b));
        add_h265_pps_derived(*p, sets.sps(p->pps_seq_parameter_set_id), unit);
        sets.put(std::move(p));
      } else if (type == 35) {
        (void)h265::parse_access_unit_delimiter(br, b);
      } else if (type == 38) {
        (void)h265::parse_filler_data(br, b);
      } else if (type == 39 || type == 40) {
        (void)h265::parse_sei_rbsp(br, b, h, sei_ctx);
      }
    } catch (const ovt::ParseError& e) {
      unit.parse_error = true;
      unit.diagnostics.push_back({e.code, "Error", e.bit_offset, 0});
    }
    finish_unit(b, unit);
    out.push_back(std::move(unit));
  }
  return out;
}

std::vector<Element> ovt_elements(std::span<const ParsedUnit> units, const std::set<std::string>& ignored,
                                  const std::set<std::string>& byte_arrays) {
  std::vector<Element> out;
  for (const ParsedUnit& u : units) {
    if (u.tree.empty()) {
      continue;
    }
    collect_elements(*flatbuffers::GetRoot<fb::SyntaxNode>(u.tree.data()), Collect{u, ignored, byte_arrays, out});
  }
  sort_elements(out);
  return out;
}

std::vector<Element> golden_elements(const nlohmann::json& golden, const std::map<std::string, std::string>& rename) {
  std::vector<Element> out;
  out.reserve(golden.size());
  std::string prev_raw;
  for (const auto& e : golden) {
    const auto raw = e.at("name").get<std::string>();
    std::string name = base_name(raw);
    if (const auto it = rename.find(name); it != rename.end()) {
      name = it->second;
    }
    Element el{e.at("unit").get<uint32_t>(), std::move(name), e.at("bit_offset").get<uint64_t>(),
               e.at("bit_length").get<uint64_t>(), e.at("value")};
    if (!out.empty() && raw == prev_raw && continues_split_read(out.back(), el)) {
      Element& head = out.back();
      head.value = (head.value.get<uint64_t>() << el.bit_length) | el.value.get<uint64_t>();
      head.bit_length += el.bit_length;
      continue;
    }
    prev_raw = raw;
    out.push_back(std::move(el));
  }
  sort_elements(out);
  return out;
}

// SPEC-ASSUMPTION(#663): a coverage-only span must start at a golden element, and the golden elements from there to
// the end of the unit must all lie inside it. Returns the index of the first golden element after the span.
template <typename Note>
size_t skip_opaque_span(const std::vector<Element>& golden, size_t i, const Element& o, const Note& note) {
  const uint64_t end = o.bit_offset + o.bit_length;
  if (i >= golden.size() || golden[i].unit != o.unit || golden[i].bit_offset != o.bit_offset) {
    note(fmt::format("unit {}: ovt {} [{}+{}] does not start at a golden element", o.unit, o.name, o.bit_offset,
                     o.bit_length));
  }
  for (; i < golden.size() && golden[i].unit == o.unit && golden[i].bit_offset >= o.bit_offset; ++i) {
    if (golden[i].bit_offset + golden[i].bit_length > end) {
      note(fmt::format("unit {}: golden {} lies beyond ovt {}", o.unit, describe(golden[i]), o.name));
    }
  }
  return i;
}

std::vector<std::string> compare_elements(const std::vector<Element>& golden, const std::vector<Element>& ovt,
                                          size_t max_lines) {
  std::vector<std::string> lines;
  size_t differences = 0;
  const auto note = [&](std::string line) {
    if (++differences <= max_lines) {
      lines.push_back(std::move(line));
    }
  };
  // Walk both lists in (unit, bit_offset) order so that one missing element shows as one difference.
  size_t i = 0;
  size_t j = 0;
  while (i < golden.size() || j < ovt.size()) {
    const Element* g = i < golden.size() ? &golden[i] : nullptr;
    const Element* o = j < ovt.size() ? &ovt[j] : nullptr;
    if (o != nullptr && o->name.starts_with(kOpaquePrefix)) {
      i = skip_opaque_span(golden, i, *o, note);
      ++j;
    } else if (g != nullptr && o != nullptr && g->unit == o->unit && g->bit_offset == o->bit_offset) {
      if (!(*g == *o)) {
        note(fmt::format("unit {}: golden {} | ovt {}", g->unit, describe(*g), describe(*o)));
      }
      ++i;
      ++j;
    } else if (o == nullptr ||
               (g != nullptr && (g->unit < o->unit || (g->unit == o->unit && g->bit_offset < o->bit_offset)))) {
      note(fmt::format("unit {}: golden {} | ovt has no element at this offset", g->unit, describe(*g)));
      ++i;
    } else {
      note(fmt::format("unit {}: golden has no element at this offset | ovt {}", o->unit, describe(*o)));
      ++j;
    }
  }
  if (differences > max_lines) {
    lines.push_back(fmt::format("... and {} more differences", differences - max_lines));
  }
  return lines;
}

std::string read_text(const std::filesystem::path& path) {
  const std::string name = path.string();
  if (name.size() > 3 && name.ends_with(".gz")) {
    // gzip is on every CI image (the metric golden tests read their goldens the same way).
    const std::string cmd = "gzip -dc '" + name + "'";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),  // NOLINT(cert-env33-c): fixed command
                                               pclose);
    if (pipe == nullptr) {
      throw std::runtime_error("cannot run gzip for " + name);
    }
    std::string text;
    std::array<char, 65536> buf{};
    size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe.get())) > 0) {
      text.append(buf.data(), n);
    }
    if (text.empty()) {
      throw std::runtime_error("empty or unreadable " + name);
    }
    return text;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open " + name);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::set<std::string> read_list(const std::filesystem::path& path) {
  std::set<std::string> out;
  std::istringstream in(read_text(path));
  std::string line;
  while (std::getline(in, line)) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const auto last = line.find_last_not_of(" \t\r");
    out.insert(line.substr(first, last - first + 1));
  }
  return out;
}

std::map<std::string, std::string> read_rename(const std::filesystem::path& path, Codec codec) {
  const auto doc = nlohmann::json::parse(read_text(path));
  const char* key = codec == Codec::kH264 ? "h264" : codec == Codec::kH265 ? "h265" : "av1";
  std::map<std::string, std::string> out;
  if (doc.contains("rename") && doc["rename"].contains(key)) {
    for (const auto& [cbs, spec] : doc["rename"][key].items()) {
      out.emplace(cbs, spec.get<std::string>());
    }
  }
  return out;
}

GstMap read_gst_map(const std::filesystem::path& path, Codec codec) {
  const auto doc = nlohmann::json::parse(read_text(path));
  const char* key = codec == Codec::kH264 ? "h264" : codec == Codec::kH265 ? "h265" : "av1";
  GstMap map;
  if (!doc.contains(key)) {
    return map;
  }
  const auto& c = doc.at(key);
  for (const auto& [s, types] : c.at("structs").items()) {
    map.structs.insert(s);
    map.unit_types[s] = types.get<std::vector<uint32_t>>();
  }
  const auto parents = c.value("qualified_parents", nlohmann::json::array());
  for (const auto& p : parents) {
    map.qualified_parents.insert(p.get<std::string>());
  }
  const auto rename = c.value("rename", nlohmann::json::object());
  for (const auto& [k, v] : rename.items()) {
    map.rename.emplace(k, v.get<std::string>());
  }
  const auto skip = c.value("skip", nlohmann::json::object());
  for (const auto& [k, v] : skip.items()) {
    map.skip.emplace(k, v.get<std::string>());
  }
  return map;
}

std::vector<std::string> gst_compare(const nlohmann::json& golden, std::span<const ParsedUnit> units, const GstMap& map,
                                     size_t max_lines) {
  std::vector<std::string> lines;
  size_t differences = 0;
  const auto note = [&](std::string line) {
    if (++differences <= max_lines) {
      lines.push_back(std::move(line));
    }
  };
  // Every unit type that carries a structure takes part in the matching.
  const auto any_struct = [&](const ParsedUnit& u) {
    return std::any_of(map.unit_types.begin(), map.unit_types.end(), [&](const auto& kv) {
      return std::find(kv.second.begin(), kv.second.end(), u.type) != kv.second.end();
    });
  };
  size_t next = 0;  // next OVT unit to match
  for (const auto& [unit_key, unit] : golden.items()) {
    const auto structure = golden_structure(unit, map);
    if (structure.empty()) {
      continue;
    }
    while (next < units.size() && !any_struct(units[next])) {
      ++next;
    }
    if (next >= units.size()) {
      note(fmt::format("golden unit {} ({}): OVT has no more units", unit_key, structure));
      break;
    }
    compare_unit(unit_key, structure, unit.at(structure), units[next++], map, note);
  }
  if (differences > max_lines) {
    lines.push_back(fmt::format("... and {} more differences", differences - max_lines));
  }
  return lines;
}

std::set<std::string> read_known(const std::filesystem::path& path, Codec codec) {
  const auto doc = nlohmann::json::parse(read_text(path));
  const char* key = codec == Codec::kH264 ? "h264" : codec == Codec::kH265 ? "h265" : "av1";
  const auto rename = read_rename(path, codec);
  std::set<std::string> out;
  if (doc.contains("known") && doc["known"].contains(key)) {
    for (const auto& name : doc["known"][key]) {
      const auto s = name.get<std::string>();
      const auto it = rename.find(s);
      out.insert(it != rename.end() ? it->second : s);
    }
  }
  return out;
}

}  // namespace ovt::test::syntax_golden
