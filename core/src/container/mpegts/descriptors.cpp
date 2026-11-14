// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MPEG-TS descriptor registry and decoder (chapter 08 §8.6.7, WI-4.205). Syntax: ISO/IEC 13818-1 §2.6 (tags
// 0x02-0x3F) and ETSI EN 300 468 §6.2 (tags 0x40-0x7F).
//
// SPEC-ASSUMPTION(#434): §8.10 lists "0x7F extension (HEVC timing/HDR)", but HEVC_timing_and_HRD_descriptor is
// extension 0x03 of the MPEG extension_descriptor (tag 0x3F), and the DVB tag 0x7F carries other extensions. Both
// tags are decoded as extension_descriptor_tag + payload; extension 0x03 of tag 0x3F is decoded in full.

#include "container/mpegts/descriptors.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace ovt::container::mpegts {

namespace {

constexpr std::string_view k13818 = "ISO/IEC 13818-1 §2.6";
constexpr std::string_view k300468 = "ETSI EN 300 468 §6.2";

// clang-format off
constexpr std::array<DescriptorInfo, 27> kTable = {{
    {0x02, "video_stream_descriptor", k13818},
    {0x03, "audio_stream_descriptor", k13818},
    {0x04, "hierarchy_descriptor", k13818},
    {0x05, "registration_descriptor", k13818},
    {0x06, "data_stream_alignment_descriptor", k13818},
    {0x07, "target_background_grid_descriptor", k13818},
    {0x08, "video_window_descriptor", k13818},
    {0x09, "CA_descriptor", k13818},
    {0x0A, "ISO_639_language_descriptor", k13818},
    {0x0B, "system_clock_descriptor", k13818},
    {0x0C, "multiplex_buffer_utilization_descriptor", k13818},
    {0x0D, "copyright_descriptor", k13818},
    {0x0E, "maximum_bitrate_descriptor", k13818},
    {0x28, "AVC_video_descriptor", k13818},
    {0x2B, "MPEG2_AAC_audio_descriptor", k13818},
    {0x38, "HEVC_video_descriptor", k13818},
    {0x3F, "extension_descriptor", k13818},
    {0x40, "network_name_descriptor", k300468},
    {0x47, "bouquet_name_descriptor", k300468},
    {0x48, "service_descriptor", k300468},
    {0x4D, "short_event_descriptor", k300468},
    {0x56, "teletext_descriptor", k300468},
    {0x58, "local_time_offset_descriptor", k300468},
    {0x59, "subtitling_descriptor", k300468},
    {0x6A, "AC-3_descriptor", k300468},
    {0x7A, "enhanced_AC-3_descriptor", k300468},
    {0x7F, "extension_descriptor", k300468},
}};
// clang-format on

// Cursor over a descriptor body that records every element it reads. A read past the end sets `failed` and returns 0.
class Reader {
 public:
  Reader(std::span<const uint8_t> body, std::vector<DescField>& out) : b_(body), out_(out) {}

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] bool more() const noexcept { return !failed_ && pos_ < b_.size() * 8U; }
  [[nodiscard]] size_t bits_left() const noexcept { return b_.size() * 8U - pos_; }

  uint64_t u(std::string name, unsigned bits) {
    if (failed_ || bits > bits_left()) {
      failed_ = true;
      return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < bits; ++i, ++pos_) {
      v = (v << 1U) | ((b_[pos_ / 8U] >> (7U - pos_ % 8U)) & 1U);
    }
    DescField f;
    f.name = std::move(name);
    f.value = v;
    f.bit_offset = static_cast<uint32_t>(16U + pos_ - bits);
    f.bit_length = bits;
    out_.push_back(std::move(f));
    return v;
  }

  void skip(unsigned bits) {
    if (failed_ || bits > bits_left()) {
      failed_ = true;
      return;
    }
    pos_ += bits;
  }

  // Three-letter or four-letter codes and other printable ASCII (language, format_identifier, country_code).
  void code(std::string name, unsigned nbytes) {
    if (failed_ || size_t{nbytes} * 8U > bits_left()) {
      failed_ = true;
      return;
    }
    DescField f;
    f.name = std::move(name);
    f.is_text = true;
    for (unsigned i = 0; i < nbytes; ++i) {
      const uint8_t c = b_[pos_ / 8U + i];
      f.text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
    }
    f.bit_offset = static_cast<uint32_t>(16U + pos_);
    f.bit_length = nbytes * 8U;
    pos_ += size_t{nbytes} * 8U;
    out_.push_back(std::move(f));
  }

  // A length byte followed by that many bytes of DVB text.
  void text8(std::string name) {
    const uint64_t n = u(name + "_length", 8);
    if (failed_ || n * 8U > bits_left()) {
      failed_ = true;
      return;
    }
    DescField f;
    f.name = std::move(name);
    f.is_text = true;
    f.text = dvb_text(b_.subspan(pos_ / 8U, static_cast<size_t>(n)));
    f.bit_offset = static_cast<uint32_t>(16U + pos_);
    f.bit_length = static_cast<uint32_t>(n * 8U);
    pos_ += static_cast<size_t>(n) * 8U;
    out_.push_back(std::move(f));
  }

  // All remaining whole bytes as DVB text (byte aligned readers only).
  void text_rest(std::string name) {
    if (failed_ || pos_ % 8U != 0) {
      failed_ = true;
      return;
    }
    DescField f;
    f.name = std::move(name);
    f.is_text = true;
    f.text = dvb_text(b_.subspan(pos_ / 8U));
    f.bit_offset = static_cast<uint32_t>(16U + pos_);
    f.bit_length = static_cast<uint32_t>(bits_left());
    pos_ = b_.size() * 8U;
    out_.push_back(std::move(f));
  }

  // All remaining whole bytes as hex (byte aligned readers only).
  void rest_hex(std::string name) {
    if (failed_ || pos_ % 8U != 0) {
      failed_ = true;
      return;
    }
    const auto rest = b_.subspan(pos_ / 8U);
    if (rest.empty()) {
      return;
    }
    DescField f;
    f.name = std::move(name);
    f.is_text = true;
    f.text = to_hex(rest);
    f.bit_offset = static_cast<uint32_t>(16U + pos_);
    f.bit_length = static_cast<uint32_t>(rest.size() * 8U);
    pos_ = b_.size() * 8U;
    out_.push_back(std::move(f));
  }

 private:
  std::span<const uint8_t> b_;
  std::vector<DescField>& out_;
  size_t pos_ = 0;
  bool failed_ = false;
};

void decode_hevc_video(Reader& r) {
  r.u("profile_space", 2);
  r.u("tier_flag", 1);
  r.u("profile_idc", 5);
  r.u("profile_compatibility_indication", 32);
  r.u("progressive_source_flag", 1);
  r.u("interlaced_source_flag", 1);
  r.u("non_packed_constraint_flag", 1);
  r.u("frame_only_constraint_flag", 1);
  r.skip(44);
  r.u("level_idc", 8);
  const bool subset = r.u("temporal_layer_subset_flag", 1) != 0;
  r.u("HEVC_still_present_flag", 1);
  r.u("HEVC_24hr_picture_present_flag", 1);
  r.u("sub_pic_params_present_flag", 1);
  r.skip(2);
  r.u("HDR_WCG_idc", 2);
  if (subset) {
    r.skip(5);
    r.u("HEVC_temporal_id_min", 3);
    r.skip(5);
    r.u("HEVC_temporal_id_max", 3);
  }
}

void decode_extension(uint8_t tag, Reader& r) {
  const uint64_t ext = r.u("extension_descriptor_tag", 8);
  if (tag == 0x3F && ext == 0x03) {  // HEVC_timing_and_HRD_descriptor
    r.u("hrd_management_valid_flag", 1);
    r.skip(6);
    if (r.u("picture_and_timing_info_present_flag", 1) != 0) {
      const bool khz90 = r.u("90kHz_flag", 1) != 0;
      r.skip(7);
      if (!khz90) {
        r.u("N", 32);
        r.u("K", 32);
      }
      r.u("num_units_in_tick", 32);
    }
    return;
  }
  r.rest_hex("extension_payload");
}

void decode_ac3(uint8_t tag, Reader& r) {
  std::array<uint64_t, 8> flags{};
  const bool eac3 = tag == 0x7A;
  static constexpr std::array<const char*, 8> kAc3Names = {"component_type_flag", "bsid_flag",      "mainid_flag",
                                                           "asvc_flag",           "mixinfoexists",  "substream1_flag",
                                                           "substream2_flag",     "substream3_flag"};
  const unsigned n = eac3 ? 8 : 4;
  for (unsigned i = 0; i < n; ++i) {
    flags[i] = r.u(kAc3Names[i], 1);
  }
  if (!eac3) {
    r.skip(4);
  }
  static constexpr std::array<const char*, 4> kAc3Fields = {"component_type", "bsid", "mainid", "asvc"};
  for (unsigned i = 0; i < 4; ++i) {
    if (flags[i] != 0) {
      r.u(kAc3Fields[i], 8);
    }
  }
  static constexpr std::array<const char*, 3> kSub = {"substream1", "substream2", "substream3"};
  for (unsigned i = 0; eac3 && i < 3; ++i) {
    if (flags[5 + i] != 0) {
      r.u(kSub[i], 8);
    }
  }
  r.rest_hex("additional_info");
}

void decode_body(uint8_t tag, Reader& r) {
  switch (tag) {
    case 0x02:
      r.u("multiple_frame_rate_flag", 1);
      r.u("frame_rate_code", 4);
      {
        const bool mpeg1_only = r.u("MPEG_1_only_flag", 1) != 0;
        r.u("constrained_parameter_flag", 1);
        r.u("still_picture_flag", 1);
        if (!mpeg1_only) {
          r.u("profile_and_level_indication", 8);
          r.u("chroma_format", 2);
          r.u("frame_rate_extension_flag", 1);
          r.skip(5);
        }
      }
      break;
    case 0x03:
      r.u("free_format_flag", 1);
      r.u("ID", 1);
      r.u("layer", 2);
      r.u("variable_rate_audio_indicator", 1);
      r.skip(3);
      break;
    case 0x04:
      r.skip(1);
      r.u("temporal_scalability_flag", 1);
      r.u("spatial_scalability_flag", 1);
      r.u("quality_scalability_flag", 1);
      r.u("hierarchy_type", 4);
      r.skip(2);
      r.u("hierarchy_layer_index", 6);
      r.u("tref_present_flag", 1);
      r.skip(1);
      r.u("hierarchy_embedded_layer_index", 6);
      r.skip(2);
      r.u("hierarchy_channel", 6);
      break;
    case 0x05:
      r.code("format_identifier", 4);
      r.rest_hex("additional_identification_info");
      break;
    case 0x06:
      r.u("alignment_type", 8);
      break;
    case 0x07:
    case 0x08:
      r.u(tag == 0x07 ? "horizontal_size" : "horizontal_offset", 14);
      r.u(tag == 0x07 ? "vertical_size" : "vertical_offset", 14);
      r.u(tag == 0x07 ? "aspect_ratio_information" : "window_priority", 4);
      break;
    case 0x09:
      r.u("CA_system_ID", 16);
      r.skip(3);
      r.u("CA_PID", 13);
      r.rest_hex("private_data_byte");
      break;
    case 0x0A:
      while (r.more()) {
        r.code("ISO_639_language_code", 3);
        r.u("audio_type", 8);
      }
      break;
    case 0x0B:
      r.u("external_clock_reference_indicator", 1);
      r.skip(1);
      r.u("clock_accuracy_integer", 6);
      r.u("clock_accuracy_exponent", 3);
      r.skip(5);
      break;
    case 0x0C:
      r.u("bound_valid_flag", 1);
      r.u("LTW_offset_lower_bound", 15);
      r.skip(1);
      r.u("LTW_offset_upper_bound", 15);
      break;
    case 0x0D:
      r.u("copyright_identifier", 32);
      r.rest_hex("additional_copyright_info");
      break;
    case 0x0E:
      r.skip(2);
      r.u("maximum_bitrate", 22);  // units of 50 bytes/s
      break;
    case 0x28:
      r.u("profile_idc", 8);
      r.u("constraint_set_flags", 6);
      r.u("AVC_compatible_flags", 2);
      r.u("level_idc", 8);
      r.u("AVC_still_present", 1);
      r.u("AVC_24_hour_picture_flag", 1);
      r.u("Frame_Packing_SEI_not_present_flag", 1);
      r.skip(5);
      break;
    case 0x2B:
      r.u("MPEG-2_AAC_profile", 8);
      r.u("MPEG-2_AAC_channel_configuration", 8);
      r.u("MPEG-2_AAC_additional_information", 8);
      break;
    case 0x38:
      decode_hevc_video(r);
      break;
    case 0x3F:
    case 0x7F:
      decode_extension(tag, r);
      break;
    case 0x40:
    case 0x47:
      r.text_rest(tag == 0x40 ? "network_name" : "bouquet_name");
      break;
    case 0x48:
      r.u("service_type", 8);
      r.text8("service_provider_name");
      r.text8("service_name");
      break;
    case 0x4D:
      r.code("ISO_639_language_code", 3);
      r.text8("event_name");
      r.text8("text");
      break;
    case 0x56:
      while (r.more()) {
        r.code("ISO_639_language_code", 3);
        r.u("teletext_type", 5);
        r.u("teletext_magazine_number", 3);
        r.u("teletext_page_number", 8);
      }
      break;
    case 0x58:
      while (r.more()) {
        r.code("country_code", 3);
        r.u("country_region_id", 6);
        r.skip(1);
        r.u("local_time_offset_polarity", 1);
        r.u("local_time_offset", 16);
        r.u("time_of_change", 40);
        r.u("next_time_offset", 16);
      }
      break;
    case 0x59:
      while (r.more()) {
        r.code("ISO_639_language_code", 3);
        r.u("subtitling_type", 8);
        r.u("composition_page_id", 16);
        r.u("ancillary_page_id", 16);
      }
      break;
    case 0x6A:
    case 0x7A:
      decode_ac3(tag, r);
      break;
    default:
      r.rest_hex("payload");
      break;
  }
}

}  // namespace

const DescriptorInfo* find_descriptor(uint8_t tag) noexcept {
  for (const DescriptorInfo& d : kTable) {
    if (d.tag == tag) {
      return &d;
    }
  }
  return nullptr;
}

std::string to_hex(std::span<const uint8_t> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string s;
  s.reserve(bytes.size() * 2);
  for (const uint8_t b : bytes) {
    s.push_back(digits[b >> 4U]);
    s.push_back(digits[b & 15U]);
  }
  return s;
}

std::string dvb_text(std::span<const uint8_t> bytes) {
  size_t i = 0;
  if (!bytes.empty() && bytes[0] == 0x10) {  // selector 0x10 + two bytes: ISO 8859 part
    i = 3;
  } else if (!bytes.empty() && bytes[0] < 0x20) {  // single-byte table selector
    i = 1;
  }
  std::string out;
  for (; i < bytes.size(); ++i) {
    const uint8_t c = bytes[i];
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) {
      continue;  // control codes (0x8A is a line break in DVB; shown as nothing)
    }
    if (c < 0x7F) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(0xC0U | (c >> 6U)));
      out.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
    }
  }
  return out;
}

std::vector<DescField> decode_descriptor(uint8_t tag, std::span<const uint8_t> body) {
  std::vector<DescField> out;
  Reader r(body, out);
  decode_body(tag, r);
  if (r.failed()) {  // body shorter than its syntax: the bytes are shown as hex
    out.clear();
    DescField f;
    f.name = "payload";
    f.is_text = true;
    f.text = to_hex(body);
    f.bit_offset = 16;
    f.bit_length = static_cast<uint32_t>(body.size() * 8U);
    out.push_back(std::move(f));
  }
  return out;
}

}  // namespace ovt::container::mpegts
