// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.084 acceptance tests 1 (units part) and 2: MV conversion (chapter 06 §6.2.3) and the blob_id grammar
// (§6.6.2).

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "ovt/cxx/model/units.hpp"

namespace {

using ovt::fb::Codec;
using ovt::fb::PlaneStage;
using ovt::model::BlobIdParts;
using ovt::model::BlobKind;

constexpr std::string_view kHashHex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

std::array<std::uint8_t, 32> test_hash() {
  std::array<std::uint8_t, 32> hash{};
  for (std::size_t i = 0; i < hash.size(); ++i) {
    hash.at(i) = static_cast<std::uint8_t>(i);
  }
  return hash;
}

std::string with_hash(std::string_view tail) {
  return std::string(kHashHex) + std::string(tail);
}

}  // namespace

TEST_CASE("test_units: MV conversion factors per codec", "[fast][model]") {
  CHECK(ovt::model::mv_to_neutral(3, Codec::H264) == 12);
  CHECK(ovt::model::mv_to_neutral(3, Codec::H265) == 12);
  CHECK(ovt::model::mv_to_neutral(3, Codec::AV1) == 6);
  CHECK(ovt::model::mv_to_neutral(-3, Codec::H264) == -12);
  CHECK(ovt::model::mv_to_neutral(-5, Codec::AV1) == -10);
  CHECK(ovt::model::mv_to_neutral(0, Codec::AV1) == 0);
  // AV1 extremes (MV_LOW / MV_UPP = ±2^14 in 1/8 units) fit after conversion.
  CHECK(ovt::model::mv_to_neutral_checked((1 << 14) - 1, Codec::AV1) == std::optional<std::int16_t>(32766));
  CHECK(ovt::model::mv_factor(Codec::H264) == 4);
  CHECK(ovt::model::mv_factor(Codec::H265) == 4);
  CHECK(ovt::model::mv_factor(Codec::AV1) == 2);
  CHECK_THROWS_AS(ovt::model::mv_factor(Codec::VP9), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::mv_to_neutral(1, Codec::Unknown), std::invalid_argument);
}

TEST_CASE("test_units: HEVC overflow clamps to 32767 and reports it", "[fast][model]") {
  // 8191 quarter samples = 32764: fits.
  CHECK(ovt::model::mv_to_neutral_checked(8191, Codec::H265) == std::optional<std::int16_t>(32764));
  // 8192 quarter samples = 32768: does not fit, clamps to 32767.
  CHECK_FALSE(ovt::model::mv_to_neutral_checked(8192, Codec::H265).has_value());
  CHECK(ovt::model::mv_to_neutral(8192, Codec::H265) == 32767);
  // -8192 = -32768 is the MVD sentinel, never a converted value: clamps to -32767.
  CHECK_FALSE(ovt::model::mv_to_neutral_checked(-8192, Codec::H265).has_value());
  CHECK(ovt::model::mv_to_neutral(-8192, Codec::H265) == -32767);
  // The full range after the 2^16 wrap (H.265 §8.5.3.2.1), and int32 extremes without overflow.
  CHECK(ovt::model::mv_to_neutral(32767, Codec::H265) == 32767);
  CHECK(ovt::model::mv_to_neutral(-32768, Codec::H265) == -32767);
  CHECK(ovt::model::mv_to_neutral(std::numeric_limits<std::int32_t>::max(), Codec::H265) == 32767);
  CHECK(ovt::model::mv_to_neutral(std::numeric_limits<std::int32_t>::min(), Codec::H264) == -32767);
}

TEST_CASE("test_units: mv_from_neutral inverts exactly or throws", "[fast][model]") {
  CHECK(ovt::model::mv_from_neutral(12, Codec::H264) == 3);
  CHECK(ovt::model::mv_from_neutral(-12, Codec::H265) == -3);
  CHECK(ovt::model::mv_from_neutral(6, Codec::AV1) == 3);
  CHECK(ovt::model::mv_from_neutral(-32766, Codec::AV1) == -16383);
  // An odd AV1 value means a producer stored 1/8-pel values unscaled (WI-0.084 notes).
  CHECK_THROWS_AS(ovt::model::mv_from_neutral(7, Codec::AV1), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::mv_from_neutral(13, Codec::H264), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::mv_from_neutral(32767, Codec::H265), std::invalid_argument);  // a clamped value
  // The "MVD not available" sentinel is not a vector.
  CHECK_THROWS_AS(ovt::model::mv_from_neutral(ovt::model::kMvdUnavailable, Codec::H264), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::mv_from_neutral(ovt::model::kMvdUnavailable, Codec::AV1), std::invalid_argument);
  for (std::int32_t v = -100; v <= 100; ++v) {
    CHECK(ovt::model::mv_from_neutral(ovt::model::mv_to_neutral(v, Codec::AV1), Codec::AV1) == v);
    CHECK(ovt::model::mv_from_neutral(ovt::model::mv_to_neutral(v, Codec::H264), Codec::H264) == v);
  }
}

TEST_CASE("test_units: mv_in_luma_samples divides by 16", "[fast][model]") {
  CHECK(ovt::model::mv_in_luma_samples(16) == 1.0);
  CHECK(ovt::model::mv_in_luma_samples(24) == 1.5);
  CHECK(ovt::model::mv_in_luma_samples(-4) == -0.25);
  CHECK(ovt::model::mv_in_luma_samples(32767) == 2047.9375);
  static_assert(ovt::model::mv_in_luma_samples(8) == 0.5);
}

TEST_CASE("test_units: blob_id builds the §6.6.2 form", "[fast][model]") {
  const auto hash = test_hash();
  CHECK(ovt::model::blob_id(hash, 142, PlaneStage::Prediction, 1) == with_hash("/142/pred/u"));
  CHECK(ovt::model::blob_id(hash, 0, PlaneStage::Reconstruction, 0) == with_hash("/0/recon/y"));
  CHECK(ovt::model::blob_id(hash, 7, PlaneStage::Residual, 2) == with_hash("/7/resid/v"));
  CHECK(ovt::model::blob_id(hash, 7, PlaneStage::PreDeblock, 3) == with_hash("/7/predeblock/a"));
  CHECK(ovt::model::blob_id(hash, 7, PlaneStage::PreSaoOrCdef, 0) == with_hash("/7/presaocdef/y"));
  CHECK(ovt::model::blob_id(hash, 7, PlaneStage::PreLoopRestoration, 0) == with_hash("/7/prelr/y"));
  CHECK(ovt::model::blob_id(hash, 7, PlaneStage::Coefficients, 0) == with_hash("/7/coeff/y"));

  BlobIdParts map;
  map.source_hash = hash;
  map.decode_index = 3;
  map.kind = BlobKind::MetricMap;
  map.metric_id = "ssim_Y2";
  CHECK(ovt::model::blob_id(map) == with_hash("/3/map-ssim_Y2/y"));

  BlobIdParts tb;
  tb.source_hash = hash;
  tb.decode_index = 3;
  tb.kind = BlobKind::TransformBlock;
  tb.tb_block_id = 13;
  tb.plane = 2;
  CHECK(ovt::model::blob_id(tb) == with_hash("/3/tb-13/v"));

  CHECK_THROWS_AS(ovt::model::blob_id(hash, -1, PlaneStage::Reconstruction, 0), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::blob_id(hash, 0, PlaneStage::Reconstruction, 4), std::invalid_argument);
  CHECK_THROWS_AS(ovt::model::blob_id(hash, 0, static_cast<PlaneStage>(7), 0), std::invalid_argument);
  map.metric_id = "has-dash";
  CHECK_THROWS_AS(ovt::model::blob_id(map), std::invalid_argument);
  map.metric_id = "";
  CHECK_THROWS_AS(ovt::model::blob_id(map), std::invalid_argument);
}

TEST_CASE("test_units: parse_blob_id accepts the grammar and round-trips", "[fast][model]") {
  const auto hash = test_hash();
  BlobIdParts parts;
  REQUIRE(ovt::model::parse_blob_id(with_hash("/142/pred/u"), parts));
  CHECK(parts.source_hash == hash);
  CHECK(parts.decode_index == 142);
  CHECK(parts.kind == BlobKind::Plane);
  CHECK(parts.stage == PlaneStage::Prediction);
  CHECK(parts.plane == 1);

  for (const std::string_view tail : {"/0/recon/y", "/2147483647/coeff/a", "/5/presaocdef/v", "/5/prelr/u",
                                      "/5/predeblock/y", "/5/resid/y", "/9/map-vmaf/y", "/9/map-A_b_9/u",
                                      "/9/map-abcdefghijklmnopqrstuvwxyz012345/y", "/9/tb-0/y", "/9/tb-4294967295/v"}) {
    const std::string text = with_hash(tail);
    BlobIdParts round;
    INFO(text);
    REQUIRE(ovt::model::parse_blob_id(text, round));
    CHECK(ovt::model::blob_id(round) == text);
  }

  REQUIRE(ovt::model::parse_blob_id(with_hash("/9/map-psnr/v"), parts));
  CHECK(parts.kind == BlobKind::MetricMap);
  CHECK(parts.metric_id == "psnr");
  REQUIRE(ovt::model::parse_blob_id(with_hash("/9/tb-13/u"), parts));
  CHECK(parts.kind == BlobKind::TransformBlock);
  CHECK(parts.tb_block_id == 13);
}

TEST_CASE("test_units: parse_blob_id rejects every invalid class", "[fast][model]") {
  // WI-0.084 acceptance test 2: a short hash.
  BlobIdParts untouched;
  untouched.decode_index = 77;
  BlobIdParts out = untouched;
  CHECK_FALSE(ovt::model::parse_blob_id("ab/1/recon/y", out));
  CHECK(out == untouched);

  const std::string upper = "000102030405060708090A0B0C0D0E0F101112131415161718191a1b1c1d1e1f";
  const std::string longer = std::string(kHashHex) + "0";
  const std::string bad_hex = "g" + std::string(kHashHex.substr(1));
  for (const std::string& text : {
           std::string{},                                            // empty
           upper + "/1/recon/y",                                     // upper-case hex
           longer + "/1/recon/y",                                    // 65 digits
           std::string(kHashHex.substr(1)) + "/1/recon/y",           // 63 digits
           bad_hex + "/1/recon/y",                                   // not hex
           std::string(kHashHex),                                    // hash only
           with_hash("/1/recon"),                                    // no plane
           with_hash("/1/recon/"),                                   // empty plane
           with_hash("//recon/y"),                                   // empty decode index
           with_hash("/-1/recon/y"),                                 // sign
           with_hash("/+1/recon/y"),                                 // sign
           with_hash("/01/recon/y"),                                 // padding
           with_hash("/1a/recon/y"),                                 // not a number
           with_hash("/2147483648/recon/y"),                         // above INT32_MAX
           with_hash("/99999999999999999999999/recon/y"),            // far above
           with_hash("/1/Recon/y"),                                  // case of the stage
           with_hash("/1/rec/y"),                                    // unknown stage
           with_hash("/1//y"),                                       // empty stage
           with_hash("/1/map-/y"),                                   // empty metric id
           with_hash("/1/map-abcdefghijklmnopqrstuvwxyz0123456/y"),  // 33 characters
           with_hash("/1/map-a-b/y"),                                // '-' in a metric id
           with_hash("/1/map-a.b/y"),                                // '.' in a metric id
           with_hash("/1/tb-/y"),                                    // empty tb id
           with_hash("/1/tb-01/y"),                                  // padded tb id
           with_hash("/1/tb-4294967296/y"),                          // above UINT32_MAX
           with_hash("/1/tb-1x/y"),                                  // not a number
           with_hash("/1/recon/Y"),                                  // case of the plane
           with_hash("/1/recon/w"),                                  // unknown plane
           with_hash("/1/recon/yy"),                                 // two characters
           with_hash("/1/recon/y/"),                                 // trailing slash
           with_hash("/1/recon/y/extra"),                            // extra segment
           with_hash("/1/recon/y\n"),                                // trailing newline
           " " + with_hash("/1/recon/y"),                            // leading space
       }) {
    INFO(text);
    BlobIdParts parts = untouched;
    CHECK_FALSE(ovt::model::parse_blob_id(text, parts));
    CHECK(parts == untouched);
  }
}
