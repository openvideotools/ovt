// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.082: core/schema/ovt_metrics.fbs compiles and matches chapter 06 §6.5.
//
// Step 2 / acceptance test 2: a MetricsReport with one MetricsResult (two FrameScores, one MapBlob, one
// PoolingResult) and one BdRateResult with Interpolator::Pchip finishes with "OVTR", passes
// VerifyMetricsReportBuffer and reads back unchanged; Interpolator::Akima == 2.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt_metrics_generated.h"

namespace {

constexpr std::string_view kMapBlobId = "3b7f0000000000000000000000000000000000000000000000000000000000e1/1/map-psnr/y";

flatbuffers::DetachedBuffer build_metrics_report() {
  flatbuffers::FlatBufferBuilder fbb;

  // MetricDescriptor
  const auto metric_id = fbb.CreateString("psnr");
  const auto display_name = fbb.CreateString("PSNR");
  const auto descriptor = ovt::fb::CreateMetricDescriptor(
      fbb, metric_id, display_name, ovt::fb::MetricKind::FullReference, ovt::fb::WorkingDomain::CodeValue,
      /*per_plane=*/true, /*higher_is_better=*/true, /*range_min=*/0.0, /*range_max=*/100.0,
      /*temporal_unit_frames=*/1);

  // Two FrameScores; the second references the MapBlob.
  const ovt::fb::FrameId frame0(0, 0);
  const ovt::fb::FrameId frame1(1, 2);
  const auto values0 = fbb.CreateVector(std::vector<double>{38.25, 41.5, 42.0});
  const auto values1 = fbb.CreateVector(std::vector<double>{36.125, 40.0, 40.75});
  const auto score_map_id = fbb.CreateString(kMapBlobId.data(), kMapBlobId.size());
  const std::vector<flatbuffers::Offset<ovt::fb::FrameScore>> frames{
      ovt::fb::CreateFrameScore(fbb, &frame0, values0),
      ovt::fb::CreateFrameScore(fbb, &frame1, values1, score_map_id, /*map_block_size=*/4),
  };
  const auto frames_vec = fbb.CreateVector(frames);

  // One MapBlob
  const auto blob_id = fbb.CreateString(kMapBlobId.data(), kMapBlobId.size());
  const auto blob_metric = fbb.CreateString("psnr");
  ovt::fb::MapBlobBuilder map(fbb);
  map.add_blob_id(blob_id);
  map.add_frame(&frame1);
  map.add_metric_id(blob_metric);
  map.add_plane(0);
  map.add_width(480);
  map.add_height(270);
  map.add_block_size(4);
  map.add_sample_type(ovt::fb::MapSampleType::F32);
  map.add_stride_bytes(1920);
  const std::vector<flatbuffers::Offset<ovt::fb::MapBlob>> maps{map.Finish()};
  const auto maps_vec = fbb.CreateVector(maps);

  // One PoolingResult
  const auto pooled = fbb.CreateVector(std::vector<double>{37.1875, 40.75, 41.375});
  ovt::fb::PoolingResultBuilder pooling(fbb);
  pooling.add_method(ovt::fb::PoolingMethod::Median);
  pooling.add_values(pooled);
  pooling.add_frames_used(2);
  const std::vector<flatbuffers::Offset<ovt::fb::PoolingResult>> poolings{pooling.Finish()};
  const auto pooling_vec = fbb.CreateVector(poolings);

  const auto engine = fbb.CreateString("ovt-kernels 1.0");
  ovt::fb::MetricsResultBuilder result(fbb);
  result.add_descriptor(descriptor);
  result.add_frames(frames_vec);
  result.add_engine(engine);
  result.add_maps(maps_vec);
  result.add_pooling(pooling_vec);
  result.add_schema_version(1);
  result.add_frames_total(2);
  result.add_frames_scored(2);
  const std::vector<flatbuffers::Offset<ovt::fb::MetricsResult>> results{result.Finish()};
  const auto results_vec = fbb.CreateVector(results);

  // One BdRateResult with Pchip (written explicitly although it is the schema default).
  const auto bd_metric = fbb.CreateString("psnr");
  ovt::fb::BdRateResultBuilder bd(fbb);
  bd.add_metric_id(bd_metric);
  bd.add_interpolator(ovt::fb::Interpolator::Pchip);
  bd.add_bd_rate_percent(-12.5);
  const std::vector<flatbuffers::Offset<ovt::fb::BdRateResult>> bd_rates{bd.Finish()};
  const auto bd_vec = fbb.CreateVector(bd_rates);

  ovt::fb::MetricsReportBuilder report(fbb);
  report.add_schema_version(1);
  report.add_results(results_vec);
  report.add_bd_rates(bd_vec);
  ovt::fb::FinishMetricsReportBuffer(fbb, report.Finish());
  return fbb.Release();
}

}  // namespace

TEST_CASE("test_metrics_schema: MetricsReport finishes as OVTR, verifies and reads back", "[schema][fast]") {
  CHECK(std::string_view(ovt::fb::MetricsReportIdentifier()) == "OVTR");
  CHECK(std::string_view(ovt::fb::MetricsReportExtension()) == "fm");

  const auto buf = build_metrics_report();
  CHECK(flatbuffers::BufferHasIdentifier(buf.data(), "OVTR"));
  CHECK(ovt::fb::MetricsReportBufferHasIdentifier(buf.data()));
  flatbuffers::Verifier verifier(buf.data(), buf.size());
  REQUIRE(ovt::fb::VerifyMetricsReportBuffer(verifier));
  // The root_type/file_identifier of the including file win over those of ovt_model.fbs (cf. WI-0.081 notes).
  flatbuffers::Verifier as_fa(buf.data(), buf.size());
  CHECK_FALSE(as_fa.VerifyBuffer<ovt::fb::FrameAnalysis>("OVTA"));

  const auto* report = ovt::fb::GetMetricsReport(buf.data());
  CHECK(report->schema_version() == 1U);
  CHECK(report->groups() == nullptr);  // absent vector = not requested

  REQUIRE(report->results() != nullptr);
  REQUIRE(report->results()->size() == 1);
  const auto* result = report->results()->Get(0);
  REQUIRE(result->descriptor() != nullptr);
  CHECK(result->descriptor()->id()->str() == "psnr");
  CHECK(result->descriptor()->kind() == ovt::fb::MetricKind::FullReference);
  CHECK(result->engine()->str() == "ovt-kernels 1.0");
  CHECK(result->frames_scored() == 2U);

  // Both FrameScore values.
  REQUIRE(result->frames() != nullptr);
  REQUIRE(result->frames()->size() == 2);
  const auto* score0 = result->frames()->Get(0);
  const auto* score1 = result->frames()->Get(1);
  CHECK(score0->frame()->decode_index() == 0);
  CHECK(score1->frame()->decode_index() == 1);
  CHECK(score1->frame()->display_index() == 2);
  REQUIRE(score0->values()->size() == 3);
  REQUIRE(score1->values()->size() == 3);
  // Values are dyadic fractions, so exact comparison is the round-trip check.
  CHECK(score0->values()->Get(0) == 38.25);
  CHECK(score0->values()->Get(1) == 41.5);
  CHECK(score0->values()->Get(2) == 42.0);
  CHECK(score1->values()->Get(0) == 36.125);
  CHECK(score1->values()->Get(1) == 40.0);
  CHECK(score1->values()->Get(2) == 40.75);
  CHECK(score0->map_blob_id() == nullptr);
  CHECK(score1->map_blob_id()->string_view() == kMapBlobId);
  CHECK(score1->map_block_size() == 4U);

  // The MapBlob id.
  REQUIRE(result->maps() != nullptr);
  REQUIRE(result->maps()->size() == 1);
  const auto* map = result->maps()->Get(0);
  CHECK(map->blob_id()->string_view() == kMapBlobId);
  CHECK(map->blob_id()->string_view() == score1->map_blob_id()->string_view());
  CHECK(map->frame()->decode_index() == 1);
  CHECK(map->metric_id()->str() == "psnr");
  CHECK(map->width() == 480U);
  CHECK(map->height() == 270U);
  CHECK(map->block_size() == 4U);
  CHECK(map->sample_type() == ovt::fb::MapSampleType::F32);

  // The PoolingResult.
  REQUIRE(result->pooling() != nullptr);
  REQUIRE(result->pooling()->size() == 1);
  const auto* pooling = result->pooling()->Get(0);
  CHECK(pooling->method() == ovt::fb::PoolingMethod::Median);
  CHECK(pooling->frames_used() == 2U);
  CHECK(pooling->scene_index() == -1);  // schema default
  REQUIRE(pooling->values()->size() == 3);
  CHECK(pooling->values()->Get(0) == 37.1875);

  // The BdRateResult.
  REQUIRE(report->bd_rates() != nullptr);
  REQUIRE(report->bd_rates()->size() == 1);
  const auto* bd = report->bd_rates()->Get(0);
  CHECK(bd->metric_id()->str() == "psnr");
  CHECK(bd->interpolator() == ovt::fb::Interpolator::Pchip);
  CHECK(bd->bd_rate_percent() == -12.5);
  CHECK(bd->plane() == 255U);     // schema default: combined
  CHECK(bd->log_base_bitrate());  // schema default
  CHECK(bd->anchor() == nullptr);
}

TEST_CASE("test_metrics_schema: enum numbering of §6.5", "[schema][fast]") {
  using ovt::fb::Interpolator;
  CHECK(static_cast<int>(Interpolator::Cubic) == 0);
  CHECK(static_cast<int>(Interpolator::Pchip) == 1);
  CHECK(static_cast<int>(Interpolator::Akima) == 2);

  using ovt::fb::PoolingMethod;
  CHECK(static_cast<int>(PoolingMethod::Mean) == 0);
  CHECK(static_cast<int>(PoolingMethod::LogMean) == 7);
  CHECK(static_cast<int>(PoolingMethod::PerScene) == 9);

  CHECK(static_cast<int>(ovt::fb::MapSampleType::F32) == 2);
  CHECK(static_cast<int>(ovt::fb::ScaleFilter::Point) == 4);
  CHECK(static_cast<int>(ovt::fb::AlignmentMode::AutoHashSearch) == 3);
  CHECK(static_cast<int>(ovt::fb::BitDepthPolicy::KeepIfEqualElseError) == 3);
  CHECK(static_cast<int>(ovt::fb::MetricKind::Bitstream) == 3);
  CHECK(static_cast<int>(ovt::fb::WorkingDomain::ModelRGB) == 4);
}
