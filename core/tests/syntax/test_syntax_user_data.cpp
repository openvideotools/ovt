// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the T.35 and user_data_unregistered registries (chapter 7 §7.3.11, WI-1.120). Payloads are
// assembled by hand, bit by bit, so each field of the spec's syntax tables is visible in the test.
//
// SPEC-ASSUMPTION(#89): see core/src/syntax/common/user_data_registry.cpp.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "syntax/common/user_data_registry.h"

namespace {

using namespace ovt::syntax;  // NOLINT(google-build-using-namespace)
using Bytes = std::vector<uint8_t>;

class BitWriter {
 public:
  BitWriter& put(uint64_t value, unsigned bits) {
    for (unsigned i = bits; i-- > 0;) {
      if (nbits_ % 8 == 0) {
        bytes_.push_back(0);
      }
      if (((value >> i) & 1U) != 0) {
        bytes_.back() = static_cast<uint8_t>(bytes_.back() | (0x80U >> (nbits_ % 8)));
      }
      ++nbits_;
    }
    return *this;
  }
  [[nodiscard]] const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
  unsigned nbits_ = 0;
};

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

Bytes text_bytes(std::string_view s) {
  return Bytes(s.begin(), s.end());
}

// ST 2094-40 body with one window: maxscl 1000/2000/3000, tone mapping and colour saturation present.
Bytes hdr10plus_body_one_window(uint8_t version = 1) {
  BitWriter w;
  w.put(4, 8).put(version, 8);                  // application_identifier, application_version
  w.put(1, 2);                                  // num_windows
  w.put(4000, 27).put(0, 1);                    // targeted_system_display_maximum_luminance, no grid
  w.put(1000, 17).put(2000, 17).put(3000, 17);  // maxscl[3]
  w.put(1500, 17);                              // average_maxrgb
  w.put(3, 4);                                  // num_distribution_maxrgb_percentiles
  w.put(1, 7).put(100, 17).put(50, 7).put(900, 17).put(99, 7).put(2800, 17);
  w.put(25, 10);  // fraction_bright_pixels
  w.put(0, 1);    // mastering_display_actual_peak_luminance_flag
  w.put(1, 1).put(0x123, 12).put(0x456, 12).put(2, 4).put(0x155, 10).put(0x2AA, 10);  // tone mapping
  w.put(1, 1).put(33, 6);                                                             // colour saturation
  return w.bytes();
}

Bytes hdr10plus_t35(const Bytes& body) {
  return cat({Bytes{0xB5, 0x00, 0x3C, 0x00, 0x01}, body});
}

}  // namespace

TEST_CASE("T.35 registry is a constexpr table", "[fast][syntax][user_data]") {
  static_assert(kT35Registry.size() == 4);
  static_assert(kT35Registry[0].provider_code == 0x003C);
  static_assert(kUuidRegistry[0].uuid[0] == 0xDC);
  REQUIRE(kT35Registry[1].enum_name == "ATSC A/53 (GA94) CEA-708 captions");
}

TEST_CASE("HDR10+ T.35 SEI: all ST 2094-40 fields", "[fast][syntax][user_data]") {
  const T35Result r = parse_t35(hdr10plus_t35(hdr10plus_body_one_window()));
  REQUIRE(r.enum_name == "HDR10+ (SMPTE ST 2094-40)");
  REQUIRE(r.kind == T35Kind::Hdr10Plus);
  REQUIRE(r.country_code == 0xB5);
  REQUIRE(r.provider_code == 0x003C);
  REQUIRE(r.provider_oriented_code == 0x0001);
  REQUIRE(r.has_hdr10plus);
  REQUIRE_FALSE(r.has_dolby_vision);
  REQUIRE(r.hdr10plus.has_value());
  const Hdr10Plus& m = *r.hdr10plus;
  REQUIRE(m.application_identifier == 4);
  REQUIRE(m.application_version == 1);
  REQUIRE(m.num_windows == 1);
  REQUIRE(m.windows.empty());
  REQUIRE(m.targeted_system_display_maximum_luminance == 4000);
  REQUIRE_FALSE(m.targeted_system_display_actual_peak_luminance.has_value());
  REQUIRE(m.processing.size() == 1);
  const Hdr10PlusProcessingWindow& w = m.processing[0];
  REQUIRE(w.maxscl[0] == 1000);
  REQUIRE(w.maxscl[1] == 2000);
  REQUIRE(w.maxscl[2] == 3000);
  REQUIRE(w.average_maxrgb == 1500);
  REQUIRE(w.distribution_maxrgb.size() == 3);
  REQUIRE(w.distribution_maxrgb[1].percentage == 50);
  REQUIRE(w.distribution_maxrgb[1].percentile == 900);
  REQUIRE(w.fraction_bright_pixels == 25);
  REQUIRE_FALSE(m.mastering_display_actual_peak_luminance.has_value());
  REQUIRE(w.tone_mapping.has_value());
  REQUIRE(w.tone_mapping->knee_point_x == 0x123);
  REQUIRE(w.tone_mapping->knee_point_y == 0x456);
  REQUIRE(w.tone_mapping->bezier_curve_anchors == std::vector<uint16_t>{0x155, 0x2AA});
  REQUIRE(w.color_saturation_weight == 33);
}

TEST_CASE("HDR10+ with three windows and both luminance grids", "[fast][syntax][user_data]") {
  BitWriter w;
  w.put(4, 8).put(0, 8).put(3, 2);
  for (uint64_t i = 1; i < 3; ++i) {  // ellipse windows 1 and 2
    w.put(10 * i, 16).put(20 * i, 16).put(30 * i, 16).put(40 * i, 16).put(50 * i, 16).put(60 * i, 16);
    w.put(7, 8).put(11, 16).put(12, 16).put(13, 16).put(i & 1U, 1);
  }
  w.put(1000, 27).put(1, 1).put(2, 5).put(3, 5);  // targeted display grid 2x3
  for (unsigned i = 0; i < 6; ++i) {
    w.put(i + 1, 4);
  }
  for (unsigned win = 0; win < 3; ++win) {
    w.put(win + 1, 17).put(win + 2, 17).put(win + 3, 17).put(100, 17).put(0, 4).put(5, 10);
  }
  w.put(1, 1).put(1, 5).put(2, 5).put(9, 4).put(10, 4);  // mastering display grid 1x2
  for (unsigned win = 0; win < 3; ++win) {
    w.put(0, 1).put(win == 2 ? 1 : 0, 1);
    if (win == 2) {
      w.put(63, 6);
    }
  }
  const T35Result r = parse_t35(hdr10plus_t35(w.bytes()));
  REQUIRE(r.hdr10plus.has_value());
  const Hdr10Plus& m = *r.hdr10plus;
  REQUIRE(m.num_windows == 3);
  REQUIRE(m.windows.size() == 2);
  REQUIRE(m.windows[1].window_upper_left_corner_x == 20);
  REQUIRE(m.windows[0].overlap_process_option == 1);
  REQUIRE(m.targeted_system_display_actual_peak_luminance->actual_peak_luminance ==
          std::vector<uint8_t>{1, 2, 3, 4, 5, 6});
  REQUIRE(m.processing.size() == 3);
  REQUIRE(m.processing[2].maxscl[2] == 5);
  REQUIRE(m.mastering_display_actual_peak_luminance->num_rows == 1);
  REQUIRE(m.mastering_display_actual_peak_luminance->actual_peak_luminance == std::vector<uint8_t>{9, 10});
  REQUIRE_FALSE(m.processing[0].tone_mapping.has_value());
  REQUIRE(m.processing[2].color_saturation_weight == 63);
}

TEST_CASE("HDR10+ in an AV1 OBU: trailing bits are stripped", "[fast][syntax][user_data]") {
  // The body ends on a byte boundary; the OBU's trailing_bits() add 0x80 and zero padding.
  Bytes body = hdr10plus_body_one_window(0);
  const size_t body_size = body.size();
  Bytes av1 = hdr10plus_t35(body);
  av1.push_back(0x80);
  av1.push_back(0x00);
  const T35Result r = parse_t35(av1, T35Carrier::Av1Metadata);
  REQUIRE(r.has_hdr10plus);
  REQUIRE(r.hdr10plus->processing[0].maxscl[2] == 3000);
  REQUIRE(strip_av1_trailing_bits(r.payload).size() == body_size);

  const Bytes plain = {0x12, 0x34};
  REQUIRE(strip_av1_trailing_bits(plain).size() == 2);  // last non-zero byte is not 0x80
  const Bytes only_trailing = {0x80, 0x00, 0x00};
  REQUIRE(strip_av1_trailing_bits(only_trailing).empty());
}

TEST_CASE("HDR10+ rejects malformed bodies", "[fast][syntax][user_data]") {
  BitWriter zero_windows;
  zero_windows.put(4, 8).put(0, 8).put(0, 2).put(0, 6);
  REQUIRE_THROWS_AS(parse_t35(hdr10plus_t35(zero_windows.bytes())), ovt::ParseError);

  Bytes truncated = hdr10plus_t35(hdr10plus_body_one_window());
  truncated.resize(truncated.size() - 4);
  REQUIRE_THROWS_AS(parse_t35(truncated), ovt::ParseError);

  BitWriter big_grid;
  big_grid.put(4, 8).put(0, 8).put(1, 2).put(0, 27).put(1, 1).put(26, 5).put(1, 5).put(0, 16);
  REQUIRE_THROWS_AS(parse_t35(hdr10plus_t35(big_grid.bytes())), ovt::ParseError);
}

TEST_CASE("Samsung provider with another application is opaque", "[fast][syntax][user_data]") {
  BitWriter other_app;
  other_app.put(5, 8).put(0, 8).put(0xAB, 8);
  T35Result r = parse_t35(hdr10plus_t35(other_app.bytes()));
  REQUIRE(r.enum_name == "T.35 provider 0x003C / 0x0001");
  REQUIRE_FALSE(r.has_hdr10plus);
  BitWriter version_two;
  version_two.put(4, 8).put(2, 8).put(0xAB, 8);
  r = parse_t35(hdr10plus_t35(version_two.bytes()));
  REQUIRE(r.enum_name == "T.35 provider 0x003C / 0x0001");
  REQUIRE_FALSE(r.has_hdr10plus);
  REQUIRE(r.payload.size() == 3);
}

TEST_CASE("ATSC A/53 GA94 cc_data counts", "[fast][syntax][user_data]") {
  BitWriter w;
  w.put(0xB5, 8).put(0x0031, 16).put(0x47413934, 32).put(0x03, 8);
  w.put(0, 1).put(1, 1).put(0, 1).put(4, 5).put(0xFF, 8);  // flags, cc_count = 4, em_data
  struct Cc {
    unsigned valid, type, d1, d2;
  };
  const std::array<Cc, 4> cc{{{1, 0, 0x14, 0x2C}, {1, 3, 0x61, 0x62}, {0, 2, 0, 0}, {1, 2, 0xC1, 0xC2}}};
  for (const auto& c : cc) {
    w.put(0x1F, 5).put(c.valid, 1).put(c.type, 2).put(c.d1, 8).put(c.d2, 8);
  }
  w.put(0xFF, 8);  // marker_bits
  const T35Result r = parse_t35(w.bytes());
  REQUIRE(r.enum_name == "ATSC A/53 (GA94) CEA-708 captions");
  REQUIRE_FALSE(r.provider_oriented_code.has_value());
  REQUIRE(r.atsc.has_value());
  REQUIRE(r.atsc->user_identifier == kGa94UserIdentifier);
  REQUIRE(r.atsc->is_cc_data);
  REQUIRE(r.atsc->process_cc_data_flag);
  REQUIRE_FALSE(r.atsc->process_em_data_flag);
  REQUIRE(r.atsc->cc_count == 4);
  REQUIRE(r.atsc->valid_608_packets == 1);
  REQUIRE(r.atsc->valid_708_packets == 2);
  REQUIRE(r.atsc->invalid_packets == 1);
}

TEST_CASE("ATSC: bar data and other types are opaque; truncated cc_data throws", "[fast][syntax][user_data]") {
  const Bytes bar = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x06, 0x01, 0x02};
  const T35Result r = parse_t35(bar);
  REQUIRE(r.atsc.has_value());
  REQUIRE_FALSE(r.atsc->is_cc_data);
  REQUIRE(r.atsc->user_data_type_code == 0x06);

  const Bytes cut = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03, 0x45, 0xFF, 0xFC, 0x14};
  REQUIRE_THROWS_AS(parse_t35(cut), ovt::ParseError);

  // Provider 0x0031 without the GA94 identifier is not ATSC captions.
  const Bytes other = {0xB5, 0x00, 0x31, 'X', 'Y', 'Z', '!', 0x03};
  REQUIRE(parse_t35(other).atsc == std::nullopt);
}

TEST_CASE("Dolby Vision in an AV1 T.35 OBU uses the 32-bit provider-oriented code", "[fast][syntax][user_data]") {
  const Bytes rpu = {0xB5, 0x00, 0x3B, 0x00, 0x00, 0x08, 0x00, 0x19, 0x80, 0x55};
  const T35Result r = parse_t35(rpu, T35Carrier::Av1Metadata);
  REQUIRE(r.enum_name == "Dolby Vision RPU in T.35 (AV1)");
  REQUIRE(r.has_dolby_vision);
  REQUIRE(r.provider_oriented_code == 0x00000800);
  REQUIRE(r.payload.size() == 3);  // opaque RPU bytes, trailing bits untouched

  const Bytes other_code = {0xB5, 0x00, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x01};
  const T35Result o = parse_t35(other_code);
  REQUIRE_FALSE(o.has_dolby_vision);
  REQUIRE(o.enum_name == "T.35 provider 0x003B / 0x0000");
}

TEST_CASE("Other T.35 countries and providers", "[fast][syntax][user_data]") {
  const Bytes vivid = {0x26, 0x00, 0x04, 0x00, 0x05, 0xAA};
  T35Result r = parse_t35(vivid);
  REQUIRE(r.enum_name == "HDR Vivid (T/UWA 005)");
  REQUIRE(r.kind == T35Kind::HdrVivid);

  const Bytes unknown_provider = {0xB5, 0x12, 0x34, 0xAB, 0xCD, 0x01};
  r = parse_t35(unknown_provider);
  REQUIRE(r.enum_name == "T.35 provider 0x1234 / 0xABCD");
  REQUIRE(r.payload.size() == 1);

  const Bytes unknown_country = {0x3C, 0x12, 0x34};
  r = parse_t35(unknown_country);
  REQUIRE(r.enum_name == "T.35 country 0x3C");
  REQUIRE_FALSE(r.provider_code.has_value());
  REQUIRE(r.payload.size() == 2);

  const Bytes extended = {0xFF, 0x07, 0x01};
  r = parse_t35(extended);
  REQUIRE(r.country_code_extension_byte == 0x07);
  REQUIRE(r.enum_name == "T.35 country 0xFF");

  REQUIRE_THROWS_AS(parse_t35(Bytes{}), ovt::ParseError);
  REQUIRE_THROWS_AS(parse_t35(Bytes{0xB5, 0x00}), ovt::ParseError);
}

TEST_CASE("user_data_unregistered: x264 options", "[fast][syntax][user_data]") {
  const Bytes uuid = {0xDC, 0x45, 0xE9, 0xBD, 0xE6, 0xD9, 0x48, 0xB7, 0x96, 0x2C, 0xD8, 0x20, 0xD9, 0x23, 0xEE, 0xEF};
  Bytes payload = cat({uuid, text_bytes("x264 - core 164 r3095 baee400 - H.264/MPEG-4 AVC codec - Copyleft 2003-2022 - "
                                        "http://www.videolan.org/x264.html - options: cabac=1 ref=3 deblock=1:0:0 "
                                        "analyse=0x3:0x113 me=hex")});
  payload.push_back(0);  // x264 NUL-terminates the string
  const UserDataUnregistered r = parse_user_data_unregistered(payload);
  REQUIRE(r.known);
  REQUIRE(r.enum_name == "x264 options");
  REQUIRE(r.option_prefix == "x264_option");
  REQUIRE(r.uuid_text == "DC45E9BD-E6D9-48B7-962C-D820D923EEEF");
  REQUIRE(r.text.has_value());
  REQUIRE(r.options.size() == 5);
  REQUIRE(r.options[0].name == "cabac");
  REQUIRE(r.options[0].value == "1");
  REQUIRE(r.options[2].name == "deblock");
  REQUIRE(r.options[2].value == "1:0:0");
  REQUIRE(r.options[3].value == "0x3:0x113");
}

TEST_CASE("user_data_unregistered: x265 options include flags without a value", "[fast][syntax][user_data]") {
  const Bytes uuid = {0x2C, 0xA2, 0xDE, 0x09, 0xB5, 0x17, 0x47, 0xDB, 0xBB, 0x55, 0xA4, 0xFE, 0x7F, 0xC2, 0xFC, 0x4E};
  const Bytes payload =
      cat({uuid, text_bytes("x265 (build 199) - H.265/HEVC codec - options: wpp ctu=64 no-open-gop")});
  const UserDataUnregistered r = parse_user_data_unregistered(payload);
  REQUIRE(r.enum_name == "x265 options");
  REQUIRE(r.option_prefix == "x265_option");
  REQUIRE(r.options.size() == 3);
  REQUIRE(r.options[0].name == "wpp");
  REQUIRE_FALSE(r.options[0].has_value);
  REQUIRE(r.options[1].value == "64");
  REQUIRE(r.options[2].name == "no-open-gop");
}

TEST_CASE("user_data_unregistered: unknown UUID and short payload", "[fast][syntax][user_data]") {
  Bytes uuid(16, 0x11);
  uuid[0] = 0xA6;
  const UserDataUnregistered text = parse_user_data_unregistered(cat({uuid, text_bytes("hello")}));
  REQUIRE_FALSE(text.known);
  REQUIRE(text.enum_name == "user data (UUID A6111111-1111-1111-1111-111111111111)");
  REQUIRE(text.text == "hello");
  REQUIRE(text.options.empty());

  const UserDataUnregistered binary = parse_user_data_unregistered(cat({uuid, Bytes{0x00, 0x01, 0xFF}}));
  REQUIRE_FALSE(binary.text.has_value());
  REQUIRE(binary.payload.size() == 3);

  REQUIRE_THROWS_AS(parse_user_data_unregistered(Bytes(15, 0)), ovt::ParseError);
  REQUIRE(parse_user_data_unregistered(Bytes(16, 0)).payload.empty());
}

TEST_CASE("Dolby Vision NAL markers", "[fast][syntax][user_data]") {
  const auto rpu = classify_hevc_unspecified_nal(62, 0);
  REQUIRE(rpu.has_value());
  REQUIRE(rpu->enum_name == "Dolby Vision RPU");
  REQUIRE(rpu->has_dolby_vision);
  REQUIRE_FALSE(classify_hevc_unspecified_nal(62, 1).has_value());
  REQUIRE(classify_hevc_unspecified_nal(63, 0)->enum_name == "Dolby Vision EL");
  REQUIRE_FALSE(classify_hevc_unspecified_nal(48, 0).has_value());
  REQUIRE_FALSE(classify_h264_nal28(false).has_value());
  REQUIRE(classify_h264_nal28(true)->has_dolby_vision);
}
