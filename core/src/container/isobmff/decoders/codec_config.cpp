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
#include "syntax/common/h273_names.h"
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

// ---- vpcC (VP Codec ISO Media File Format Binding §2.3) --------------------------------------------------------

constexpr std::string_view kRefVpcc = "VP Codec ISO Media File Format Binding §2.3";

std::string_view vp_chroma_name(uint64_t c) {
  constexpr std::array<std::string_view, 4> kNames{"4:2:0 vertical", "4:2:0 colocated with luma (0,0)", "4:2:2",
                                                   "4:4:4"};
  return c < kNames.size() ? kNames.at(c) : std::string_view{};
}

uint64_t decode_vpcc(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefVpcc);
  if (const int v = fb.node().version; v != 1) {
    fb.diag(Severity::Warning, "CNT-1003", "unknown vpcC version " + std::to_string(v) + "; record not decoded");
    return 0;
  }
  fb.u8("profile");
  const uint8_t level = fb.u8("level");
  fb.derived(std::to_string(level / 10U) + "." + std::to_string(level % 10U));
  fb.derived(std::to_string(fb.u("bitDepth", 4)) + "-bit");
  fb.derived(vp_chroma_name(fb.u("chromaSubsampling", 3)));
  fb.derived(fb.u("videoFullRangeFlag", 1) != 0 ? "full range" : "video range");
  fb.derived(syntax::h273_colour_primaries_name(fb.u8("colourPrimaries")));
  fb.derived(syntax::h273_transfer_characteristics_name(fb.u8("transferCharacteristics")));
  fb.derived(syntax::h273_matrix_coefficients_name(fb.u8("matrixCoefficients")));
  const uint16_t init_size = fb.u16("codecIntializationDataSize");
  fb.bytes("codecIntializationData", init_size);
  const std::string_view entry = sample_entry_of(ctx);
  if (init_size != 0 && (entry == "vp08" || entry == "vp09")) {
    fb.diag(Severity::Warning, "CNT-1022",
            "codecIntializationDataSize is " + std::to_string(init_size) + " for " + std::string(entry));
  }
  return 0;
}

// ---- esds (14496-14 §6.7; 14496-1 §7.2.6.5-7.2.6.7) and AudioSpecificConfig (14496-3 §1.6.2.1) ----------------

constexpr std::string_view kRefEsds = "ISO/IEC 14496-1:2010 §7.2.6";
constexpr std::string_view kRefAsc = "ISO/IEC 14496-3:2019 §1.6.2.1";
constexpr unsigned kMaxDescriptorDepth = 4;

std::string_view oti_name(uint64_t oti) {
  switch (oti) {
    case 0x20:
      return "MPEG-4 Visual";
    case 0x21:
      return "AVC";
    case 0x40:
      return "AAC (MPEG-4 Audio)";
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
      return "MPEG-2 Visual";
    case 0x66:
    case 0x67:
    case 0x68:
      return "MPEG-2 AAC";
    case 0x69:
      return "MP3 (ISO/IEC 13818-3)";
    case 0x6A:
      return "MPEG-1 Visual";
    case 0x6B:
      return "MP3 (ISO/IEC 11172-3)";
    case 0x6C:
      return "JPEG";
    case 0xA5:
      return "AC-3";
    case 0xA6:
      return "E-AC-3";
    case 0xAD:
      return "Opus";
    case 0xDD:
      return "Vorbis";
    default:
      return {};
  }
}

std::string_view stream_type_name(uint64_t t) {
  constexpr std::array<std::string_view, 12> kNames{"forbidden",
                                                    "ObjectDescriptorStream",
                                                    "ClockReferenceStream",
                                                    "SceneDescriptionStream",
                                                    "VisualStream",
                                                    "AudioStream",
                                                    "MPEG7Stream",
                                                    "IPMPStream",
                                                    "ObjectContentInfoStream",
                                                    "MPEGJStream",
                                                    "Interaction Stream",
                                                    "IPMPToolStream"};
  return t < kNames.size() ? kNames.at(t) : std::string_view{};
}

std::string_view descriptor_name(uint64_t tag) {
  switch (tag) {
    case 0x03:
      return "ES_Descriptor";
    case 0x04:
      return "DecoderConfigDescriptor";
    case 0x05:
      return "DecoderSpecificInfo";
    case 0x06:
      return "SLConfigDescriptor";
    default:
      return "descriptor";
  }
}

std::string_view aot_name(uint64_t aot) {
  switch (aot) {
    case 1:
      return "AAC Main";
    case 2:
      return "AAC LC";
    case 3:
      return "AAC SSR";
    case 4:
      return "AAC LTP";
    case 5:
      return "SBR";
    case 6:
      return "AAC Scalable";
    case 29:
      return "PS";
    case 42:
      return "USAC";
    default:
      return {};
  }
}

constexpr std::array<uint32_t, 13> kSamplingFrequencies{96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                        22050, 16000, 12000, 11025, 8000,  7350};

// The descriptor and AAC readers below keep to the end of the enclosing descriptor (`end`, in reader bits).
struct Bounded {
  FieldBuilder& fb;
  BitReader& br;
  uint64_t end;
  [[nodiscard]] uint64_t left() const { return end > br.bit_pos() ? end - br.bit_pos() : 0; }
  [[nodiscard]] uint64_t peek(unsigned n) const {
    if (left() < n) {
      return ~uint64_t{0};
    }
    BitReader copy = br;
    return copy.u(n);
  }
};

uint64_t sampling_frequency(Bounded& b, std::string_view index_name, std::string_view value_name) {
  const uint64_t index = b.fb.u(index_name, 4);
  if (index == 15) {
    const uint64_t hz = b.fb.u(value_name, 24);
    b.fb.derived(std::to_string(hz) + " Hz");
    return hz;
  }
  if (index < kSamplingFrequencies.size()) {
    b.fb.derived(std::to_string(kSamplingFrequencies.at(index)) + " Hz");
    return kSamplingFrequencies.at(index);
  }
  return 0;
}

uint64_t audio_object_type(Bounded& b, std::string_view name) {
  uint64_t aot = b.fb.u(name, 5);
  if (aot == 31) {
    aot = 32 + b.fb.u(std::string(name) + "Ext", 6);
  }
  b.fb.derived(aot_name(aot));
  return aot;
}

// GASpecificConfig (14496-3 §4.4.1) for the AAC object types; stops at a program_config_element (channelConfiguration
// 0), which stays in the descriptor's remaining bytes.
bool ga_specific_config(Bounded& b, uint64_t aot, uint64_t channels) {
  constexpr std::array<uint64_t, 12> kGa{1, 2, 3, 4, 6, 7, 17, 19, 20, 21, 22, 23};
  if (std::ranges::find(kGa, aot) == kGa.end() || channels == 0 || b.left() < 3) {
    return false;
  }
  b.fb.group("GASpecificConfig");
  b.fb.u("frameLengthFlag", 1);
  if (b.fb.u("dependsOnCoreCoder", 1) != 0) {
    b.fb.u("coreCoderDelay", 14);
  }
  const uint64_t extension = b.fb.u("extensionFlag", 1);
  b.fb.end();
  return extension == 0;
}

// The backward-compatible explicit SBR / PS signalling (14496-3 §1.6.5.2, syncExtensionType 0x2B7 and 0x548).
std::string sync_extension(Bounded& b, uint64_t aot) {
  if (aot == 5 || aot == 29 || b.peek(11) != 0x2B7) {
    return {};
  }
  std::string profile;
  b.fb.group("syncExtension");
  b.fb.u("syncExtensionType", 11);
  const uint64_t ext_aot = audio_object_type(b, "extensionAudioObjectType");
  if (ext_aot == 5 && b.left() >= 1) {
    if (b.fb.u("sbrPresentFlag", 1) != 0) {
      profile = "HE-AAC";
      sampling_frequency(b, "extensionSamplingFrequencyIndex", "extensionSamplingFrequency");
      if (b.peek(11) == 0x548) {
        b.fb.u("syncExtensionType", 11);
        if (b.fb.u("psPresentFlag", 1) != 0) {
          profile = "HE-AAC v2";
        }
      }
    }
  }
  b.fb.end();
  return profile;
}

void audio_specific_config(Bounded& b) {
  b.fb.group("AudioSpecificConfig");
  uint64_t aot = audio_object_type(b, "audioObjectType");
  sampling_frequency(b, "samplingFrequencyIndex", "samplingFrequency");
  const uint64_t channels = b.fb.u("channelConfiguration", 4);
  constexpr std::array<std::string_view, 8> kLayouts{
      "defined in the PCE", "mono", "stereo", "3.0", "4.0", "5.0", "5.1", "7.1"};
  if (channels < kLayouts.size()) {
    b.fb.derived(kLayouts.at(channels));
  }
  std::string profile;
  if (aot == 5 || aot == 29) {  // explicit hierarchical signalling
    profile = aot == 29 ? "HE-AAC v2" : "HE-AAC";
    sampling_frequency(b, "extensionSamplingFrequencyIndex", "extensionSamplingFrequency");
    aot = audio_object_type(b, "audioObjectType2");
  }
  if (ga_specific_config(b, aot, channels)) {
    if (const std::string s = sync_extension(b, aot); !s.empty()) {
      profile = s;
    }
  }
  b.fb.end();
  if (!profile.empty()) {
    b.fb.derived(profile);
  }
}

// Fills the remaining bits of a descriptor: a sub-byte run, then whole bytes.
void rest_of_descriptor(Bounded& b, std::string_view name) {
  if (const uint64_t odd = b.left() % 8U; odd != 0) {
    b.fb.u("alignment_bits", static_cast<unsigned>(odd));
  }
  if (b.left() > 0) {
    b.fb.bytes(name, b.left() / 8U);
  }
}

// expandable size (14496-1 §8.3.3): up to four bytes, 7 bits each, the top bit says another byte follows.
uint64_t expandable_size(Bounded& b) {
  const std::span<const uint8_t> rest = b.br.remaining_bytes();
  unsigned n = 0;
  uint64_t size = 0;
  for (; n < 4 && n < rest.size(); ++n) {
    size = (size << 7U) | (rest[n] & 0x7FU);
    if ((rest[n] & 0x80U) == 0) {
      ++n;
      break;
    }
  }
  b.fb.u("sizeOfInstance", 8U * std::max(n, 1U));
  b.fb.derived(std::to_string(size) + " bytes");
  return size;
}

// Descriptors nest (ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo); kMaxDescriptorDepth bounds the
// recursion.
// NOLINTBEGIN(misc-no-recursion)
void descriptor(Bounded& parent, uint64_t oti, unsigned depth);

void descriptor_children(Bounded& b, uint64_t oti, unsigned depth) {
  while (b.left() >= 16) {
    descriptor(b, oti, depth + 1);
  }
  rest_of_descriptor(b, "trailing_bytes");
}

void es_descriptor(Bounded& b, unsigned depth) {
  b.fb.u16("ES_ID");
  const uint64_t dep = b.fb.u("streamDependenceFlag", 1);
  const uint64_t url = b.fb.u("URL_Flag", 1);
  const uint64_t ocr = b.fb.u("OCRstreamFlag", 1);
  b.fb.u("streamPriority", 5);
  if (dep != 0) {
    b.fb.u16("dependsOn_ES_ID");
  }
  if (url != 0) {
    b.fb.str_fixed("URLstring", b.fb.u8("URLlength"));
  }
  if (ocr != 0) {
    b.fb.u16("OCR_ES_Id");
  }
  descriptor_children(b, 0, depth);
}

void decoder_config(Bounded& b, unsigned depth) {
  const uint8_t oti = b.fb.u8("objectTypeIndication");
  b.fb.derived(oti_name(oti));
  b.fb.derived(stream_type_name(b.fb.u("streamType", 6)));
  b.fb.u("upStream", 1);
  b.fb.reserved(1, 1);
  b.fb.u24("bufferSizeDB");
  b.fb.u32("maxBitrate");
  b.fb.u32("avgBitrate");
  descriptor_children(b, oti, depth);
}

void descriptor(Bounded& parent, uint64_t oti, unsigned depth) {
  FieldBuilder& fb = parent.fb;
  const std::span<const uint8_t> peek = parent.br.remaining_bytes();
  const uint64_t tag = peek.empty() ? 0 : peek[0];
  fb.group(descriptor_name(tag));
  fb.u8("tag");
  Bounded self{fb, parent.br, parent.end};
  const uint64_t size = expandable_size(self);
  if (size * 8U > self.left()) {
    fb.diag(Severity::Error, "CNT-1014",
            std::string(descriptor_name(tag)) + " size " + std::to_string(size) + " exceeds its container");
  } else {
    self.end = parent.br.bit_pos() + size * 8U;
  }
  if (depth < kMaxDescriptorDepth && tag == 0x03) {
    es_descriptor(self, depth);
  } else if (depth < kMaxDescriptorDepth && tag == 0x04) {
    decoder_config(self, depth);
  } else if (tag == 0x05 && oti == 0x40 && self.left() >= 16) {
    const SpecRefScope asc_ref(fb, kRefAsc);
    audio_specific_config(self);
    rest_of_descriptor(self, "remaining_bytes");
  } else if (tag == 0x06 && self.left() >= 8) {
    fb.u8("predefined");
    rest_of_descriptor(self, "SLConfig_fields");
  } else {
    rest_of_descriptor(self, "data");
  }
  fb.end();
}

// NOLINTEND(misc-no-recursion)

uint64_t decode_esds(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefEsds);
  Bounded b{fb, br, br.bit_pos() + br.bits_left()};
  while (b.left() >= 16) {
    descriptor(b, 0, 0);
  }
  rest_of_descriptor(b, "trailing_bytes");
  return 0;
}

// ---- dOps (Encapsulation of Opus in ISO Base Media File Format §4.3.2) -----------------------------------------

constexpr std::string_view kRefDops = "Encapsulation of Opus in ISO Base Media File Format §4.3.2";

uint64_t decode_dops(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefDops);
  if (const uint8_t v = fb.u8("Version"); v != 0) {
    fb.diag(Severity::Warning, "CNT-1003", "unknown dOps Version " + std::to_string(v) + "; record not decoded");
    return 0;
  }
  const uint8_t channels = fb.u8("OutputChannelCount");
  const uint16_t pre_skip = fb.u16("PreSkip");
  fb.derived(std::to_string(pre_skip / 48.0).substr(0, 6) + " ms at 48 kHz");
  fb.derived(std::to_string(fb.u32("InputSampleRate")) + " Hz");
  const int16_t gain = fb.i16("OutputGain");
  fb.derived(std::to_string(gain / 256.0).substr(0, 7) + " dB");
  if (fb.u8("ChannelMappingFamily") != 0) {
    fb.u8("StreamCount");
    fb.u8("CoupledCount");
    fb.array("ChannelMapping", channels);
    for (uint32_t i = 0; i < channels; ++i) {
      fb.u8("ChannelMapping[" + std::to_string(i) + "]");
    }
    fb.end();
  }
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
OVT_BOX_DECODER("vpcC", "*", &decode_vpcc)
OVT_BOX_DECODER("esds", "*", &decode_esds)
OVT_BOX_DECODER("dOps", "*", &decode_dops)

}  // namespace ovt::container::isobmff
