// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group C part 1: the codec configuration records (chapter 08 §8.3.3 rows 1-6, WI-1.207). `avcC`,
// `hvcC` and `av1C` carry parameter sets: each one is handed to the syntax module through parse_parameter_set()
// (core/src/syntax/common/parameter_set.h) and its tree is attached under the field that holds the bytes
// (`Field.children`), with spans moved into the box (bits from the box start, emulation prevention bytes counted).
// Diagnostics of those trees go onto the box. Derived text goes into `Field.svalue` (SPEC-ASSUMPTION(#415),
// field_builder.h); a derived value without a source field is a `<name>.derived` sibling (#567).

#include "container/isobmff/decoders/codec_config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt_container_generated.h"
#include "ovt_model_generated.h"
#include "syntax/common/parameter_set.h"
#include "syntax/common/syntax_node_buffer.h"

namespace ovt::container::isobmff {

namespace {

// Stamps `spec_ref` on every top-level field a decoder adds, also when the decoder throws (CNT-1002). Same helper
// as in sample_entries.cpp.
class SpecRefScope {
 public:
  SpecRefScope(FieldBuilder& fb, std::string_view ref) : fb_(fb), first_(fb.node().fields.size()), ref_(ref) {}
  SpecRefScope(const SpecRefScope&) = delete;
  SpecRefScope& operator=(const SpecRefScope&) = delete;
  SpecRefScope(SpecRefScope&&) = delete;
  SpecRefScope& operator=(SpecRefScope&&) = delete;
  ~SpecRefScope() {
    auto& fields = fb_.node().fields;
    for (size_t i = first_; i < fields.size(); ++i) {
      if (fields[i] && fields[i]->spec_ref.empty()) {
        fields[i]->spec_ref = std::string(ref_);
      }
    }
  }

 private:
  FieldBuilder& fb_;
  size_t first_;
  std::string_view ref_;
};

// SPEC-ASSUMPTION(#567): a `<name>.derived` String sibling with an empty span after the last top-level field.
void derived_sibling(FieldBuilder& fb, std::string_view name, std::string_view text) {
  auto& fields = fb.node().fields;
  uint64_t at = static_cast<uint64_t>(fb.node().header_length) * 8U;
  if (!fields.empty() && fields.back() && fields.back()->span) {
    at = fields.back()->span->bit_offset() + fields.back()->span->bit_length();
  }
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(name) + ".derived";
  f->kind = fb::SyntaxValueKind::String;
  f->svalue = std::string(text);
  f->span = std::make_unique<fb::BitSpan>(at, 0);
  fields.push_back(std::move(f));
}

std::string_view sample_entry_of(const BoxContext& ctx) {
  return {ctx.sample_entry.data(), ctx.sample_entry.size()};
}

// A configurationVersion other than 1: 14496-15 §5.3.3.1.3 / §8.3.3.1.3 tell readers not to decode the record.
bool version_known(FieldBuilder& fb, uint64_t version, std::string_view what) {
  if (version == 1) {
    return true;
  }
  fb.diag(Severity::Warning, "CNT-1003",
          "unknown " + std::string(what) + " " + std::to_string(version) + "; record not decoded");
  return false;
}

// ---- attaching a syntax tree under a Bytes field -------------------------------------------------------------

// Moves syntax-tree spans (RBSP bits from the unit start) into the box: raw bits (EPBs counted) + the field start.
class SpanMap {
 public:
  SpanMap(uint64_t base_bits, std::span<const uint32_t> epbs) : base_(base_bits), epbs_(epbs) {}
  [[nodiscard]] std::unique_ptr<fb::BitSpan> map(const BitSpan& s) const {
    const uint64_t start = rbsp_bit_to_raw_bit(s.bit_offset, epbs_);
    const uint64_t end = rbsp_bit_to_raw_bit(s.bit_offset + s.bit_length, epbs_);
    const uint64_t len = s.bit_length == 0 ? 0 : end - start;
    return std::make_unique<fb::BitSpan>(base_ + start, static_cast<uint32_t>(std::min<uint64_t>(len, UINT32_MAX)));
  }

 private:
  uint64_t base_;
  std::span<const uint32_t> epbs_;
};

std::string node_text(const syntax::SyntaxNodeData& n) {
  if (!n.svalue.empty()) {
    return std::string(n.svalue);
  }
  std::string text(n.enum_name);
  if (!n.derived.empty()) {
    text += (text.empty() ? "" : "; ") + std::string(n.derived);
  }
  return text;
}

// The trees nest at most SyntaxBuilder::kMaxDepth levels, so the recursion below is bounded.
// NOLINTBEGIN(misc-no-recursion)
void collect_diagnostics(const syntax::SyntaxNodeData& n, const SpanMap& m, std::string_view where, fb::BoxNodeT& box) {
  for (const syntax::DiagnosticData* d = n.first_diag; d != nullptr; d = d->next) {
    auto out = std::make_unique<fb::DiagnosticT>();
    out->severity = static_cast<fb::Severity>(d->severity);
    out->code = std::string(d->code);
    out->message = std::string(where) + ": " + std::string(d->message);
    out->span = m.map(d->span);
    out->spec_ref = std::string(d->spec_ref);
    box.diagnostics.push_back(std::move(out));
  }
  for (const syntax::SyntaxNodeData* c = n.first_child; c != nullptr; c = c->next_sibling) {
    collect_diagnostics(*c, m, where, box);
  }
}

std::unique_ptr<fb::FieldT> to_field(const syntax::SyntaxNodeData& n, const SpanMap& m) {
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(n.name);
  if (n.array_element && n.array_index >= 0) {
    f->name += "[" + std::to_string(n.array_index) + "]";
  }
  f->kind = static_cast<fb::SyntaxValueKind>(n.kind);
  f->uvalue = n.kind == syntax::ValueKind::Bool ? static_cast<uint64_t>(n.bvalue) : n.uvalue;
  f->ivalue = n.ivalue;
  f->svalue = node_text(n);
  f->bytes.assign(n.bytes.begin(), n.bytes.end());
  f->span = m.map(n.span);
  f->spec_ref = std::string(n.spec_ref);
  for (const syntax::SyntaxNodeData* c = n.first_child; c != nullptr; c = c->next_sibling) {
    f->children.push_back(to_field(*c, m));
  }
  return f;
}

// NOLINTEND(misc-no-recursion)

// Reads `len` bytes into a Bytes field named `name`, parses them as one parameter set / OBU and attaches the tree.
// Returns the unit type (nal_unit_type / obu_type) the syntax module read, or -1 when nothing was parsed.
int unit_field(FieldBuilder& fb, std::string_view name, uint64_t len, fb::Codec codec,
               syntax::ParameterSetContext& psc) {
  const std::span<const uint8_t> bytes = fb.bytes(name, len);
  fb::FieldT* field = fb.last_field();
  if (bytes.empty() || field == nullptr || !field->span) {
    return -1;
  }
  syntax::ParameterSetResult r = syntax::parse_parameter_set(codec, bytes, &psc);
  const syntax::SyntaxNodeData* root = r.tree.root();
  if (root == nullptr) {
    return -1;
  }
  fb.derived("→ syntax tree");
  const SpanMap m(field->span->bit_offset(), r.epb_positions);
  for (const syntax::SyntaxNodeData* c = root->first_child; c != nullptr; c = c->next_sibling) {
    field->children.push_back(to_field(*c, m));
  }
  collect_diagnostics(*root, m, name, fb.node());
  return static_cast<int>(r.unit_type);
}

// ---- avcC (14496-15 §5.3.3.1.2) --------------------------------------------------------------------------------

constexpr std::string_view kRefAvcc = "ISO/IEC 14496-15:2022 §5.3.3.1.2";

std::string_view avc_profile_name(uint64_t p) {
  switch (p) {
    case 66:
      return "Baseline";
    case 77:
      return "Main";
    case 88:
      return "Extended";
    case 100:
      return "High";
    case 110:
      return "High 10";
    case 122:
      return "High 4:2:2";
    case 244:
      return "High 4:4:4 Predictive";
    default:
      return {};
  }
}

std::string avc_level_text(uint64_t level) {
  if (level == 9) {
    return "1b";
  }
  return std::to_string(level / 10U) + "." + std::to_string(level % 10U);
}

std::string_view chroma_format_name(uint64_t c) {
  constexpr std::array<std::string_view, 4> kNames{"4:0:0", "4:2:0", "4:2:2", "4:4:4"};
  return kNames.at(c & 3U);
}

// One list of length-prefixed NAL units: `count` elements named `<element>[i]` under `<list>`, each with
// `<length_name> u16` and `<unit_name> bytes`. Returns the number of units the list holds.
void avc_nal_list(FieldBuilder& fb, BitReader& br, uint64_t count, std::string_view list, std::string_view element,
                  std::string_view length_name, std::string_view unit_name, syntax::ParameterSetContext& psc) {
  fb.array(list, count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!FieldBuilder::materialise(i, static_cast<uint32_t>(count))) {
      br.skip_bytes(br.u(16));
      continue;
    }
    fb.array_element(element, i);
    const uint16_t len = fb.u16(length_name);
    (void)unit_field(fb, unit_name, len, fb::Codec::H264, psc);
    fb.end();
  }
  fb.end();
}

uint64_t decode_avcc(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefAvcc);
  if (!version_known(fb, fb.u8("configurationVersion"), "configurationVersion")) {
    return 0;
  }
  const uint8_t profile = fb.u8("AVCProfileIndication");
  fb.derived(avc_profile_name(profile));
  fb.u8("profile_compatibility");
  fb.derived(avc_level_text(fb.u8("AVCLevelIndication")));
  fb.reserved(6, 0x3F);
  const uint64_t length_size = fb.u("lengthSizeMinusOne", 2);
  fb.derived("NAL length size = " + std::to_string(length_size + 1U));
  fb.reserved(3, 0x7);
  syntax::ParameterSetContext psc;
  const uint64_t num_sps = fb.u("numOfSequenceParameterSets", 5);
  avc_nal_list(fb, br, num_sps, "sequenceParameterSets", "sequenceParameterSet", "sequenceParameterSetLength",
               "sequenceParameterSetNALUnit", psc);
  const uint8_t num_pps = fb.u8("numOfPictureParameterSets");
  avc_nal_list(fb, br, num_pps, "pictureParameterSets", "pictureParameterSet", "pictureParameterSetLength",
               "pictureParameterSetNALUnit", psc);
  const std::string_view entry = sample_entry_of(ctx);
  if ((entry == "avc3" || entry == "avc4") && num_sps + num_pps > 0) {
    fb.diag(Severity::Info, "CNT-1020",
            std::string(entry) + " entry carries " + std::to_string(num_sps + num_pps) + " parameter sets in avcC");
  }
  // The extension exists for these profiles only, and many writers leave it out even then (§8.3.3 pitfall).
  const bool ext_profile = profile == 100 || profile == 110 || profile == 122 || profile == 144;
  if (ext_profile && br.bits_left() >= 32) {
    fb.reserved(6, 0x3F);
    fb.derived(chroma_format_name(fb.u("chroma_format", 2)));
    fb.reserved(5, 0x1F);
    fb.derived(std::to_string(fb.u("bit_depth_luma_minus8", 3) + 8U) + "-bit");
    fb.reserved(5, 0x1F);
    fb.derived(std::to_string(fb.u("bit_depth_chroma_minus8", 3) + 8U) + "-bit");
    const uint8_t num_ext = fb.u8("numOfSequenceParameterSetExt");
    avc_nal_list(fb, br, num_ext, "sequenceParameterSetExts", "sequenceParameterSetExt",
                 "sequenceParameterSetExtLength", "sequenceParameterSetExtNALUnit", psc);
  }
  return 0;
}

// ---- hvcC (14496-15 §8.3.3.1.2) --------------------------------------------------------------------------------

constexpr std::string_view kRefHvcc = "ISO/IEC 14496-15:2022 §8.3.3.1.2";

std::string_view hevc_profile_name(uint64_t p) {
  switch (p) {
    case 1:
      return "Main";
    case 2:
      return "Main 10";
    case 3:
      return "Main Still Picture";
    case 4:
      return "Format Range Extensions";
    case 9:
      return "Screen Content Coding Extensions";
    default:
      return {};
  }
}

// general_level_idc = 30 × level: 93 → "3.1", 120 → "4".
std::string hevc_level_text(uint64_t idc) {
  std::string text = std::to_string(idc / 30U);
  if (idc % 30U != 0) {
    text += "." + std::to_string((idc % 30U) / 3U);
  }
  return text;
}

// general_constraint_indicator_flags (48 bits, H.265 §7.3.3 general_progressive_source_flag onwards), MSB first.
std::string constraint_text(uint64_t flags) {
  constexpr std::array<std::string_view, 13> kNames{"progressive_source",
                                                    "interlaced_source",
                                                    "non_packed_constraint",
                                                    "frame_only_constraint",
                                                    "max_12bit",
                                                    "max_10bit",
                                                    "max_8bit",
                                                    "max_422chroma",
                                                    "max_420chroma",
                                                    "max_monochrome",
                                                    "intra",
                                                    "one_picture_only",
                                                    "lower_bit_rate"};
  std::string text;
  for (size_t i = 0; i < kNames.size(); ++i) {
    if (((flags >> (47U - i)) & 1U) != 0) {
      text += (text.empty() ? "" : "|") + std::string(kNames.at(i));
    }
  }
  return text;
}

std::string_view hevc_nal_type_name(uint64_t t) {
  switch (t) {
    case 32:
      return "VPS_NUT";
    case 33:
      return "SPS_NUT";
    case 34:
      return "PPS_NUT";
    case 39:
      return "PREFIX_SEI_NUT";
    case 40:
      return "SUFFIX_SEI_NUT";
    default:
      return {};
  }
}

std::string_view parallelism_name(uint64_t p) {
  constexpr std::array<std::string_view, 4> kNames{"mixed or unknown", "slice-based", "tile-based", "wavefront-based"};
  return kNames.at(p & 3U);
}

void hvcc_array(FieldBuilder& fb, BitReader& br, uint32_t index, syntax::ParameterSetContext& psc) {
  fb.array_element("array", index);
  fb.u("array_completeness", 1);
  fb.reserved(1);
  fb.derived(hevc_nal_type_name(fb.u("NAL_unit_type", 6)));
  const uint16_t num = fb.u16("numNalus");
  fb.array("nalus", num);
  for (uint32_t j = 0; j < num; ++j) {
    if (!FieldBuilder::materialise(j, num)) {
      br.skip_bytes(br.u(16));
      continue;
    }
    fb.array_element("nalu", j);
    const uint16_t len = fb.u16("nalUnitLength");
    (void)unit_field(fb, "nalUnit", len, fb::Codec::H265, psc);
    fb.end();
  }
  fb.end();
  fb.end();
}

uint64_t decode_hvcc(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefHvcc);
  if (!version_known(fb, fb.u8("configurationVersion"), "configurationVersion")) {
    return 0;
  }
  fb.u("general_profile_space", 2);
  fb.derived(fb.u("general_tier_flag", 1) != 0 ? "High tier" : "Main tier");
  fb.derived(hevc_profile_name(fb.u("general_profile_idc", 5)));
  fb.u32("general_profile_compatibility_flags");
  fb.derived(constraint_text(fb.u("general_constraint_indicator_flags", 48)));
  fb.derived(hevc_level_text(fb.u8("general_level_idc")));
  fb.reserved(4, 0xF);
  fb.u("min_spatial_segmentation_idc", 12);
  fb.reserved(6, 0x3F);
  fb.derived(parallelism_name(fb.u("parallelismType", 2)));
  fb.reserved(6, 0x3F);
  fb.derived(chroma_format_name(fb.u("chromaFormat", 2)));
  fb.reserved(5, 0x1F);
  fb.derived(std::to_string(fb.u("bitDepthLumaMinus8", 3) + 8U) + "-bit");
  fb.reserved(5, 0x1F);
  fb.derived(std::to_string(fb.u("bitDepthChromaMinus8", 3) + 8U) + "-bit");
  const uint16_t rate = fb.u16("avgFrameRate");
  fb.derived(rate == 0 ? std::string("unspecified") : std::to_string(rate / 256.0).substr(0, 6) + " fps");
  fb.u("constantFrameRate", 2);
  fb.u("numTemporalLayers", 3);
  fb.u("temporalIdNested", 1);
  fb.derived("NAL length size = " + std::to_string(fb.u("lengthSizeMinusOne", 2) + 1U));
  const uint8_t num_arrays = fb.u8("numOfArrays");
  syntax::ParameterSetContext psc;
  fb.array("arrays", num_arrays);
  for (uint32_t i = 0; i < num_arrays; ++i) {
    hvcc_array(fb, br, i, psc);
  }
  fb.end();
  const std::string_view entry = sample_entry_of(ctx);
  if (num_arrays == 0 && (entry == "hev1" || entry == "hev2" || entry == "dvhe")) {
    derived_sibling(fb, "parameter_sets", "parameter sets in-band");
  }
  return 0;
}

// ---- av1C (AV1-ISOBMFF §2.3) -----------------------------------------------------------------------------------

constexpr std::string_view kRefAv1c = "AV1 Codec ISO Media File Format Binding §2.3";

// One OBU of configOBUs: its byte length and type, from the OBU header (AV1 §5.3.1) and leb128 obu_size.
struct ObuSlice {
  size_t size = 0;
  uint8_t type = 0;
};

std::vector<ObuSlice> split_obus(std::span<const uint8_t> data) {
  std::vector<ObuSlice> out;
  size_t pos = 0;
  while (pos < data.size()) {
    const uint8_t hdr = data[pos];
    ObuSlice s;
    s.type = static_cast<uint8_t>((hdr >> 3U) & 0xFU);
    size_t header = 1U + (((hdr >> 2U) & 1U) != 0 ? 1U : 0U);
    const size_t left = data.size() - pos;
    s.size = left;
    if (((hdr >> 1U) & 1U) != 0 && header <= left) {
      uint64_t value = 0;
      size_t i = 0;
      for (; i < 8 && header + i < left; ++i) {
        const uint8_t byte = data[pos + header + i];
        value |= static_cast<uint64_t>(byte & 0x7FU) << (7U * i);
        if ((byte & 0x80U) == 0) {
          ++i;
          break;
        }
      }
      header += i;
      if (value <= left - std::min(header, left)) {
        s.size = header + static_cast<size_t>(value);
      }
    }
    s.size = std::max<size_t>(s.size, 1);
    out.push_back(s);
    pos += s.size;
  }
  return out;
}

std::string_view av1_profile_name(uint64_t p) {
  constexpr std::array<std::string_view, 3> kNames{"Main", "High", "Professional"};
  return p < kNames.size() ? kNames.at(p) : std::string_view{};
}

uint64_t decode_av1c(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefAv1c);
  if (fb.u("marker", 1) != 1) {
    fb.diag(Severity::Info, "CNT-1007", "av1C marker is 0 (expected 1)");
  }
  if (!version_known(fb, fb.u("version", 7), "av1C version")) {
    return 0;
  }
  const uint64_t profile = fb.u("seq_profile", 3);
  fb.derived(av1_profile_name(profile));
  const uint64_t level = fb.u("seq_level_idx_0", 5);
  fb.derived(level == 31 ? std::string("unconstrained")
                         : std::to_string(2U + (level >> 2U)) + "." + std::to_string(level & 3U));
  fb.u("seq_tier_0", 1);
  const uint64_t high = fb.u("high_bitdepth", 1);
  const uint64_t twelve = fb.u("twelve_bit", 1);
  derived_sibling(fb, "bit_depth", high == 0 ? "8" : (twelve == 0 ? "10" : "12"));
  const uint64_t mono = fb.u("monochrome", 1);
  const uint64_t sx = fb.u("chroma_subsampling_x", 1);
  const uint64_t sy = fb.u("chroma_subsampling_y", 1);
  std::string_view chroma = "4:4:4";
  if (mono != 0) {
    chroma = "4:0:0";
  } else if (sx != 0) {
    chroma = sy != 0 ? "4:2:0" : "4:2:2";
  }
  derived_sibling(fb, "chroma_format", chroma);
  fb.u("chroma_sample_position", 2);
  fb.reserved(3);
  if (fb.u("initial_presentation_delay_present", 1) != 0) {
    fb.u("initial_presentation_delay_minus_one", 4);
  } else {
    fb.reserved(4);
  }
  const std::span<const uint8_t> rest = br.remaining_bytes();
  const std::vector<ObuSlice> obus = split_obus(rest);
  syntax::ParameterSetContext psc;
  fb.array("configOBUs", obus.size());
  for (uint32_t i = 0; i < obus.size(); ++i) {
    const std::string name = "obu[" + std::to_string(i) + "]";
    (void)unit_field(fb, name, obus[i].size, fb::Codec::AV1, psc);
    if (obus[i].type != 1 && obus[i].type != 5) {
      fb.diag(Severity::Error, "CNT-1021",
              "configOBUs[" + std::to_string(i) + "] has obu_type " + std::to_string(obus[i].type));
    }
  }
  fb.end();
  return 0;
}

}  // namespace

bool is_codec_config_fourcc(std::string_view fourcc) noexcept {
  constexpr std::array<std::string_view, 9> kConfigs{"avcC", "hvcC", "av1C", "vpcC", "esds",
                                                     "dOps", "dvcC", "dvvC", "dvwC"};
  return std::ranges::find(kConfigs, fourcc) != kConfigs.end();
}

std::vector<fb::CodecPrivateT> codec_private_of(const io::RandomAccessFile& file, const fb::BoxNodeT& sample_entry) {
  std::vector<fb::CodecPrivateT> out;
  for (const auto& child : sample_entry.children) {
    if (!child || !child->file_span || !is_codec_config_fourcc(child->fourcc)) {
      continue;
    }
    const uint64_t offset = child->file_span->offset();
    const uint64_t size = child->file_span->length();
    if (size < child->header_length) {
      continue;
    }
    fb::CodecPrivateT cp;
    cp.fourcc = child->fourcc;
    cp.box_offset = offset;
    cp.bytes.resize(size - child->header_length);
    const size_t got = file.read(offset + child->header_length, cp.bytes.size(), cp.bytes.data());
    cp.bytes.resize(got);
    out.push_back(std::move(cp));
  }
  return out;
}

OVT_BOX_DECODER("avcC", "*", &decode_avcc)
OVT_BOX_DECODER("hvcC", "*", &decode_hvcc)
OVT_BOX_DECODER("av1C", "*", &decode_av1c)

}  // namespace ovt::container::isobmff
