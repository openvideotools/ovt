// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.402 (chapter 10 §10.11): every row of the §10.3.3 tables, the guess fallbacks, range, MDCV order, CLL and the
// once-per-stream RND-0020/RND-0021 rule.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <libplacebo/colorspace.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "color/h273_map.h"
#include "ovt/color.h"

namespace {

using ovt::render::detail::color_description_hash;
using ovt::render::detail::format_notice;
using ovt::render::detail::H273Field;
using ovt::render::detail::H273NoticeLog;
using ovt::render::detail::H273Notices;
using ovt::render::detail::map_color_description;
using ovt::render::detail::map_matrix;
using ovt::render::detail::map_primaries;
using ovt::render::detail::map_transfer;

constexpr int kHdW = 1920;
constexpr int kHdH = 1080;

ovt_ColorDescription description(uint8_t primaries, uint8_t transfer, uint8_t matrix) {
  ovt_ColorDescription c{};
  c.struct_size = sizeof(ovt_ColorDescription);
  c.primaries = primaries;
  c.transfer = transfer;
  c.matrix = matrix;
  c.chroma_format = 1;
  c.bit_depth = 10;
  return c;
}

}  // namespace

TEST_CASE("render_test_h273_map: primaries_table", "[fast][render][h273]") {
  const std::vector<std::pair<uint8_t, pl_color_primaries>> table = {
      {1, PL_COLOR_PRIM_BT_709},      {4, PL_COLOR_PRIM_BT_470M},    {5, PL_COLOR_PRIM_BT_601_625},
      {6, PL_COLOR_PRIM_BT_601_525},  {7, PL_COLOR_PRIM_BT_601_525}, {8, PL_COLOR_PRIM_FILM_C},
      {9, PL_COLOR_PRIM_BT_2020},     {10, PL_COLOR_PRIM_CIE_1931},  {11, PL_COLOR_PRIM_DCI_P3},
      {12, PL_COLOR_PRIM_DISPLAY_P3}, {22, PL_COLOR_PRIM_EBU_3213},
  };
  for (const auto& [code, want] : table) {
    INFO("primaries " << int{code});
    H273Notices notes;
    // A small picture size proves the table wins over the size guess.
    CHECK(map_primaries(code, 720, 480, &notes) == want);
    CHECK(notes.count == 0);
  }
}

TEST_CASE("render_test_h273_map: primaries_guess", "[fast][render][h273]") {
  // WI-1.402 acceptance test 2: primaries 2 at 1920×1080 → BT.709.
  H273Notices notes;
  CHECK(map_primaries(2, kHdW, kHdH, &notes) == PL_COLOR_PRIM_BT_709);
  CHECK(map_primaries(0, kHdW, kHdH, &notes) == PL_COLOR_PRIM_BT_709);
  CHECK(map_primaries(2, 720, 576, &notes) == PL_COLOR_PRIM_BT_601_625);
  CHECK(map_primaries(2, 720, 480, &notes) == PL_COLOR_PRIM_BT_601_525);
  CHECK(notes.count == 0);  // unspecified codes are silent (#237)
  // Reserved / unsupported codes guess too, and owe an RND-0020.
  for (const uint8_t code : {uint8_t{3}, uint8_t{13}, uint8_t{21}, uint8_t{23}, uint8_t{255}}) {
    INFO("primaries " << int{code});
    H273Notices n;
    CHECK(map_primaries(code, kHdW, kHdH, &n) == pl_color_primaries_guess(kHdW, kHdH));
    REQUIRE(n.count == 1);
    CHECK(std::string(n.items[0].code) == "RND-0020");
    CHECK(n.items[0].field == H273Field::Primaries);
    CHECK(n.items[0].value == code);
  }
}

TEST_CASE("render_test_h273_map: transfer_table", "[fast][render][h273]") {
  const std::vector<std::pair<uint8_t, pl_color_transfer>> table = {
      {1, PL_COLOR_TRC_BT_1886}, {6, PL_COLOR_TRC_BT_1886}, {14, PL_COLOR_TRC_BT_1886}, {15, PL_COLOR_TRC_BT_1886},
      {4, PL_COLOR_TRC_GAMMA22}, {5, PL_COLOR_TRC_GAMMA28}, {7, PL_COLOR_TRC_BT_1886},  {8, PL_COLOR_TRC_LINEAR},
      {13, PL_COLOR_TRC_SRGB},   {16, PL_COLOR_TRC_PQ},     {17, PL_COLOR_TRC_ST428},   {18, PL_COLOR_TRC_HLG},
  };
  for (const auto& [code, want] : table) {
    INFO("transfer " << int{code});
    H273Notices notes;
    CHECK(map_transfer(code, &notes) == want);
    CHECK(notes.count == 0);
  }
  // 11 (xvYCC) and 12 (BT.1361): BT.1886 + RND-0020.
  for (const uint8_t code : {uint8_t{11}, uint8_t{12}}) {
    INFO("transfer " << int{code});
    H273Notices notes;
    CHECK(map_transfer(code, &notes) == PL_COLOR_TRC_BT_1886);
    REQUIRE(notes.count == 1);
    CHECK(format_notice(notes.items[0]) ==
          "RND-0020 unknown H.273 code " + std::to_string(code) + " for transfer, assuming BT.1886");
  }
}

TEST_CASE("render_test_h273_map: transfer_guess", "[fast][render][h273]") {
  H273Notices notes;
  CHECK(map_transfer(0, &notes) == PL_COLOR_TRC_BT_1886);
  CHECK(map_transfer(2, &notes) == PL_COLOR_TRC_BT_1886);
  CHECK(notes.count == 0);
  for (const uint8_t code : {uint8_t{3}, uint8_t{9}, uint8_t{10}, uint8_t{19}, uint8_t{200}}) {
    INFO("transfer " << int{code});
    H273Notices n;
    CHECK(map_transfer(code, &n) == PL_COLOR_TRC_BT_1886);
    CHECK(n.count == 1);
  }
}

TEST_CASE("render_test_h273_map: matrix_table", "[fast][render][h273]") {
  const std::vector<std::pair<uint8_t, pl_color_system>> table = {
      {0, PL_COLOR_SYSTEM_RGB},        {1, PL_COLOR_SYSTEM_BT_709},     {5, PL_COLOR_SYSTEM_BT_601},
      {6, PL_COLOR_SYSTEM_BT_601},     {7, PL_COLOR_SYSTEM_SMPTE_240M}, {8, PL_COLOR_SYSTEM_YCGCO},
      {9, PL_COLOR_SYSTEM_BT_2020_NC}, {10, PL_COLOR_SYSTEM_BT_2020_C},
  };
  for (const auto& [code, want] : table) {
    INFO("matrix " << int{code});
    H273Notices notes;
    CHECK(map_matrix(code, 1, 720, 480, &notes) == want);
    CHECK(notes.count == 0);
  }
  SECTION("ICtCp follows the transfer") {
    H273Notices notes;
    CHECK(map_matrix(14, 16, kHdW, kHdH, &notes) == PL_COLOR_SYSTEM_BT_2100_PQ);
    CHECK(map_matrix(14, 18, kHdW, kHdH, &notes) == PL_COLOR_SYSTEM_BT_2100_HLG);
    CHECK(notes.count == 0);
    CHECK(map_matrix(14, 1, kHdW, kHdH, &notes) == PL_COLOR_SYSTEM_BT_2100_PQ);  // #237
    CHECK(notes.count == 1);
  }
  SECTION("chromaticity-derived matrices render as BT.709 with RND-0021") {
    for (const uint8_t code : {uint8_t{12}, uint8_t{13}}) {
      H273Notices notes;
      CHECK(map_matrix(code, 1, 720, 480, &notes) == PL_COLOR_SYSTEM_BT_709);
      REQUIRE(notes.count == 1);
      CHECK(format_notice(notes.items[0]) == "RND-0021 matrix coefficients " + std::to_string(code) +
                                                 " (chromaticity-derived) are unsupported; rendered approximately "
                                                 "as BT.709");
    }
  }
}

TEST_CASE("render_test_h273_map: matrix_guess", "[fast][render][h273]") {
  H273Notices notes;
  CHECK(map_matrix(2, 1, kHdW, kHdH, &notes) == PL_COLOR_SYSTEM_BT_709);
  CHECK(map_matrix(2, 1, 720, 480, &notes) == PL_COLOR_SYSTEM_BT_601);
  CHECK(notes.count == 0);
  for (const uint8_t code : {uint8_t{3}, uint8_t{4}, uint8_t{11}, uint8_t{15}, uint8_t{255}}) {
    INFO("matrix " << int{code});
    H273Notices n;
    CHECK(map_matrix(code, 1, 720, 576, &n) == pl_color_system_guess_ycbcr(720, 576));
    REQUIRE(n.count == 1);
    CHECK(format_notice(n.items[0]) ==
          "RND-0020 unknown H.273 code " + std::to_string(code) + " for matrix, assuming BT.601");
  }
}

TEST_CASE("render_test_h273_map: range_and_alpha", "[fast][render][h273]") {
  ovt_ColorDescription c = description(1, 1, 1);
  CHECK(map_color_description(c, kHdW, kHdH).repr.levels == PL_COLOR_LEVELS_LIMITED);
  c.full_range = 1;
  const auto m = map_color_description(c, kHdW, kHdH);
  CHECK(m.repr.levels == PL_COLOR_LEVELS_FULL);
  CHECK(m.repr.sys == PL_COLOR_SYSTEM_BT_709);
  CHECK(m.repr.dovi == nullptr);
  CHECK(m.repr.bits.color_depth == 0);  // set by frame upload (§10.3.1), not here
  CHECK(m.space.primaries == PL_COLOR_PRIM_BT_709);
  CHECK(m.space.transfer == PL_COLOR_TRC_BT_1886);
#if PL_API_VER >= 344
  CHECK(m.repr.alpha == PL_ALPHA_NONE);
#else
  CHECK(m.repr.alpha == PL_ALPHA_UNKNOWN);
#endif
}

TEST_CASE("render_test_h273_map: mdcv_order", "[fast][render][h273]") {
  // §10.3.3 "mdcv order": the chapter 7 WI-2.172 x265 PQ vector, already R, G, B.
  ovt_ColorDescription c = description(9, 16, 9);
  c.mdcv_primaries[0][0] = 34000;
  c.mdcv_primaries[0][1] = 16000;
  c.mdcv_primaries[1][0] = 13250;
  c.mdcv_primaries[1][1] = 34500;
  c.mdcv_primaries[2][0] = 7500;
  c.mdcv_primaries[2][1] = 3000;
  c.mdcv_white_point[0] = 15635;
  c.mdcv_white_point[1] = 16450;
  c.mdcv_max_luminance = 10000000;  // 1000 cd/m²
  c.mdcv_min_luminance = 50;        // 0.005 cd/m²
  c.max_cll = 1000;
  c.max_fall = 400;
  const auto m = map_color_description(c, 3840, 2160);
  const pl_hdr_metadata& hdr = m.space.hdr;
  CHECK(hdr.prim.red.x == Catch::Approx(0.680F));
  CHECK(hdr.prim.red.y == Catch::Approx(0.320F));
  CHECK(hdr.prim.green.x == Catch::Approx(0.265F));
  CHECK(hdr.prim.green.y == Catch::Approx(0.690F));
  CHECK(hdr.prim.blue.x == Catch::Approx(0.150F));
  CHECK(hdr.prim.blue.y == Catch::Approx(0.060F));
  CHECK(hdr.prim.white.x == Catch::Approx(0.3127F));
  CHECK(hdr.prim.white.y == Catch::Approx(0.3290F));
  CHECK(hdr.max_luma == Catch::Approx(1000.0F));
  CHECK(hdr.min_luma == Catch::Approx(0.005F));
  CHECK(hdr.max_cll == Catch::Approx(1000.0F));  // already cd/m²
  CHECK(hdr.max_fall == Catch::Approx(400.0F));
  CHECK(m.space.primaries == PL_COLOR_PRIM_BT_2020);
  CHECK(m.space.transfer == PL_COLOR_TRC_PQ);
  CHECK(m.repr.sys == PL_COLOR_SYSTEM_BT_2020_NC);
  CHECK(m.notices.count == 0);
}

TEST_CASE("render_test_h273_map: absent_hdr_metadata_is_zero", "[fast][render][h273]") {
  // 0 = absent: libplacebo infers the luminance range from the transfer.
  const auto m = map_color_description(description(9, 16, 9), 3840, 2160);
  CHECK(m.space.hdr.max_luma == 0.0F);
  CHECK(m.space.hdr.min_luma == 0.0F);
  CHECK(m.space.hdr.max_cll == 0.0F);
  CHECK(m.space.hdr.prim.red.x == 0.0F);
}

TEST_CASE("render_test_h273_map: notice_once_per_stream", "[fast][render][h273]") {
  const ovt_ColorDescription bad = description(3, 12, 13);
  const auto m = map_color_description(bad, kHdW, kHdH);
  REQUIRE(m.notices.count == 3);
  H273NoticeLog log;
  const std::vector<std::string> first = log.first_seen(bad, m);
  REQUIRE(first.size() == 3);
  CHECK(first[0] ==
        "RND-0021 matrix coefficients 13 (chromaticity-derived) are unsupported; rendered approximately as "
        "BT.709");
  CHECK(first[1] == "RND-0020 unknown H.273 code 3 for primaries, assuming BT.709");
  CHECK(first[2] == "RND-0020 unknown H.273 code 12 for transfer, assuming BT.1886");
  CHECK(log.first_seen(bad, m).empty());  // the same stream again: silent

  // A different description (another stream) logs again; a clean one never logs.
  ovt_ColorDescription other = bad;
  other.max_cll = 100;
  CHECK(color_description_hash(other) != color_description_hash(bad));
  CHECK(log.first_seen(other, map_color_description(other, kHdW, kHdH)).size() == 3);
  const ovt_ColorDescription clean = description(1, 1, 1);
  CHECK(log.first_seen(clean, map_color_description(clean, kHdW, kHdH)).empty());
}

TEST_CASE("render_test_h273_map: hash_covers_every_field", "[fast][render][h273]") {
  const ovt_ColorDescription base = description(9, 16, 9);
  const uint64_t h = color_description_hash(base);
  CHECK(color_description_hash(base) == h);  // deterministic
  auto changed = [&](auto mutate) {
    ovt_ColorDescription c = base;
    mutate(c);
    return color_description_hash(c) != h;
  };
  CHECK(changed([](ovt_ColorDescription& c) { c.full_range = 1; }));
  CHECK(changed([](ovt_ColorDescription& c) { c.chroma_siting = 3; }));
  CHECK(changed([](ovt_ColorDescription& c) { c.mdcv_primaries[2][1] = 1; }));
  CHECK(changed([](ovt_ColorDescription& c) { c.mdcv_white_point[1] = 1; }));
  CHECK(changed([](ovt_ColorDescription& c) { c.mdcv_min_luminance = 1; }));
  CHECK(changed([](ovt_ColorDescription& c) { c.max_fall = 1; }));
  CHECK(changed([](ovt_ColorDescription& c) { c.dv_level = 1; }));
}
