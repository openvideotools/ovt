// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the StreamIndex adapter for SelectionModel, the FrameTable and StreamIndexModel (WI-1.455a; chapter 11
// §11.3.1, §11.4.1). Acceptance test 2 of the WI names `syn_h264_gop60_1080p`, which the oracle image produces; until
// the fixture's index is available to the GUI tests, SPEC-ASSUMPTION(#240) the range-statistics test below builds an
// equivalent 60-frame-GOP index in memory (frame sizes, pts in the track time base) and checks the status line
// against an independent computation, exact to one tick.

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "models/frame_table.h"
#include "models/selection_model.h"
#include "models/stream_index_adapter.h"
#include "models/stream_index_model.h"
#include "ovt_model_generated.h"
#include "qt_test_app.h"

namespace {

using namespace ovt::app;  // NOLINT(google-build-using-namespace): test file

struct SyntheticFrame {
  std::int64_t pts;
  std::uint64_t bytes;
  int qp;
  bool rap;
  int tid;
};

// Serializes a StreamIndex whose frame i (decode order) has one AUD unit and one slice unit, laid out as Annex B
// (4-byte start codes), and `display_order` given or derived from pts.
std::vector<std::uint8_t> build_index(const std::vector<SyntheticFrame>& frames, std::int32_t tb_num,
                                      std::int32_t tb_den, bool with_samples = false) {
  ovt::fb::StreamIndexT index;
  index.codec = ovt::fb::Codec::H264;
  index.schema_version = 1;
  std::vector<int> by_pts(frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    by_pts[i] = static_cast<int>(i);
  }
  std::stable_sort(by_pts.begin(), by_pts.end(), [&](int a, int b) {
    return frames[static_cast<std::size_t>(a)].pts < frames[static_cast<std::size_t>(b)].pts;
  });
  std::vector<int> display_of(frames.size());
  for (std::size_t slot = 0; slot < by_pts.size(); ++slot) {
    display_of[static_cast<std::size_t>(by_pts[slot])] = static_cast<int>(slot);
  }
  index.display_order = by_pts;

  std::uint64_t offset = 100;  // some bytes before the first start code
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto fi = std::make_unique<ovt::fb::FrameInfoT>();
    fi->id = std::make_unique<ovt::fb::FrameId>(static_cast<int>(i), display_of[i]);
    fi->pts = frames[i].pts;
    fi->dts = static_cast<std::int64_t>(i);
    fi->timebase_num = tb_num;
    fi->timebase_den = tb_den;
    fi->size_bytes = frames[i].bytes;
    fi->qp_base = static_cast<std::int16_t>(frames[i].qp);
    fi->is_rap = frames[i].rap;
    fi->shown = true;
    fi->temporal_id = static_cast<std::uint8_t>(frames[i].tid);
    fi->type = frames[i].rap ? ovt::fb::FrameType::I : ovt::fb::FrameType::P;

    // unit 2i: AUD (non-VCL), unit 2i+1: slice (VCL)
    for (int k = 0; k < 2; ++k) {
      auto u = std::make_unique<ovt::fb::UnitT>();
      u->index = static_cast<std::uint32_t>(2 * i + static_cast<std::size_t>(k));
      const std::uint64_t payload = k == 0 ? 2 : (frames[i].bytes > 8 ? frames[i].bytes - 8 : 1);
      u->prefix_span = std::make_unique<ovt::fb::ByteSpan>(offset, 4);
      u->file_span = std::make_unique<ovt::fb::ByteSpan>(offset + 4, payload);
      offset += 4 + payload;
      u->type_name = k == 0 ? "AUD" : "SLICE";
      u->frame = std::make_unique<ovt::fb::FrameId>(k == 0 ? -1 : static_cast<int>(i), k == 0 ? -1 : display_of[i]);
      if (k == 1) {
        u->epb_positions = {3, 9};
      }
      u->sample_index = with_samples ? static_cast<std::int32_t>(i) : -1;
      fi->units.push_back(u->index);
      index.units.push_back(std::move(u));
    }
    index.frames.push_back(std::move(fi));
  }
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &index));
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// Frames of an IPPP stream with a GOP of 60: an I frame every 60, sizes depend on the position.
std::vector<SyntheticFrame> gop60(std::size_t n, std::int64_t pts_step) {
  std::vector<SyntheticFrame> frames;
  for (std::size_t i = 0; i < n; ++i) {
    const bool rap = i % 60 == 0;
    frames.push_back({static_cast<std::int64_t>(i) * pts_step, rap ? 60000U + i : 900U + (i * 37U) % 400U,
                      rap ? 22 : 24 + static_cast<int>(i % 7), rap, 0});
  }
  return frames;
}

}  // namespace

TEST_CASE("StreamIndexHandle verifies the bytes", "[app][timeline][adapter]") {
  CHECK(StreamIndexHandle::adopt({}) == nullptr);
  CHECK(StreamIndexHandle::adopt(std::vector<std::uint8_t>(64, 0xFF)) == nullptr);
  auto bytes = build_index(gop60(4, 1), 1, 60);
  bytes.resize(bytes.size() / 2);  // truncated
  CHECK(StreamIndexHandle::adopt(std::move(bytes)) == nullptr);
  CHECK(StreamIndexHandle::adopt(build_index(gop60(4, 1), 1, 60)) != nullptr);
}

TEST_CASE("the adapter answers the five SelectionIndex lookups from a StreamIndex", "[app][timeline][adapter]") {
  // Decode order I P B B with display order I B B P: pts 0, 3, 1, 2.
  const std::vector<SyntheticFrame> frames{
      {0, 5000, 20, true, 0}, {3, 800, 26, false, 0}, {1, 300, 30, false, 1}, {2, 310, 30, false, 1}};
  const SelectionIndex* index = nullptr;
  std::unique_ptr<StreamIndexAdapter> adapter;
  {
    auto handle = StreamIndexHandle::adopt(build_index(frames, 1, 30, /*with_samples=*/true));
    REQUIRE(handle != nullptr);
    adapter = std::make_unique<StreamIndexAdapter>(handle);
  }  // the adapter keeps the bytes alive on its own
  index = adapter.get();

  SECTION("unit") {
    const auto u = index->unit(1);  // slice of frame 0
    REQUIRE(u.has_value());
    CHECK(u->file_span.offset == 100 + 4 + 2 + 4);
    CHECK(u->frame == FrameId{0, 0});
    CHECK(u->sample_index == 0);
    REQUIRE(u->epb_positions.size() == 2);
    CHECK(u->epb_positions[0] == 3);
    CHECK(u->epb_positions[1] == 9);
    const auto aud = index->unit(0);
    REQUIRE(aud.has_value());
    CHECK(aud->frame.decode_index == -1);
    CHECK(aud->epb_positions.empty());
    CHECK_FALSE(index->unit(-1).has_value());
    CHECK_FALSE(index->unit(8).has_value());
  }
  SECTION("resolve_frame fills both indices from either") {
    CHECK(index->resolve_frame(FrameId{1, -1}) == std::optional<FrameId>(FrameId{1, 3}));
    CHECK(index->resolve_frame(FrameId{-1, 1}) ==
          std::optional<FrameId>(FrameId{2, 1}));  // display slot 1 = pts 1 = frame 2
    CHECK(index->resolve_frame(FrameId{3, 99}) == std::optional<FrameId>(FrameId{3, 2}));
    CHECK_FALSE(index->resolve_frame(FrameId{4, -1}).has_value());
    CHECK_FALSE(index->resolve_frame(FrameId{-1, 4}).has_value());
    CHECK_FALSE(index->resolve_frame(FrameId{-1, -1}).has_value());
  }
  SECTION("first_vcl_unit skips the access unit delimiter") {
    CHECK(index->first_vcl_unit(FrameId{0, -1}) == 1);
    CHECK(index->first_vcl_unit(FrameId{-1, 3}) == 3);  // display slot 3 = frame 1
    CHECK(index->first_vcl_unit(FrameId{9, -1}) == -1);
  }
  SECTION("sample span is the union of its units including prefixes") {
    const auto s = index->sample(2);
    REQUIRE(s.has_value());
    CHECK(s->frame == FrameId{2, 1});
    CHECK(s->first_unit == 4);
    CHECK(s->span.offset == index->unit(4)->file_span.offset - 4);
    CHECK(s->span.offset + s->span.length == index->unit(5)->file_span.offset + index->unit(5)->file_span.length);
    CHECK_FALSE(index->sample(4).has_value());
    CHECK_FALSE(index->sample(-1).has_value());
  }
  SECTION("unit_at_byte finds the unit whose prefix or payload contains the offset") {
    const auto u = index->unit(3).value();
    CHECK(index->unit_at_byte(u.file_span.offset) == 3);
    CHECK(index->unit_at_byte(u.file_span.offset - 4) == 3);  // start code
    CHECK(index->unit_at_byte(u.file_span.offset + u.file_span.length - 1) == 3);
    CHECK(index->unit_at_byte(u.file_span.offset + u.file_span.length) == 4);  // next unit's start code
    CHECK(index->unit_at_byte(99) == -1);
    CHECK(index->unit_at_byte(1U << 30U) == -1);
  }
}

TEST_CASE("SelectionModel derives from a StreamIndex through the adapter", "[app][timeline][adapter]") {
  ovt::app::testing::qt_app();
  const std::vector<SyntheticFrame> frames{{0, 5000, 20, true, 0}, {1, 800, 26, false, 0}};
  StreamIndexModel model;
  model.setIndex(StreamIndexHandle::adopt(build_index(frames, 1, 30)));
  SelectionModel selection;
  selection.setIndex(model.selectionIndex());
  selection.setFrame(FrameId{1, -1}, QStringLiteral("test"));
  CHECK(selection.frame_display_index() == 1);
  CHECK(selection.unit_index() == 3);  // first VCL unit of frame 1
  CHECK(selection.byte_offset() == static_cast<qlonglong>(model.selectionIndex()->unit(3)->file_span.offset));
}

TEST_CASE("range statistics: bitrate is Σbytes×8 / Δpts from the time base, exact to one tick",
          "[app][timeline][stats]") {
  // 90 kHz track, 60 fps: 1500 ticks per frame. The drag covers frames 120–240.
  const auto frames = gop60(300, 1500);
  FrameTable table{StreamIndexHandle::adopt(build_index(frames, 1, 90000))->index()};
  const auto stats = table.range_stats(120, 240);
  REQUIRE(stats.has_value());
  CHECK(stats->frames == 121);
  std::uint64_t bytes = 0;
  double qp = 0;
  for (std::size_t i = 120; i <= 240; ++i) {
    bytes += frames[i].bytes;
    qp += frames[i].qp;
  }
  CHECK(stats->bytes == bytes);
  CHECK(stats->ticks == 121 * 1500);  // (pts[240] − pts[120]) + one frame duration
  CHECK(stats->seconds == 121.0 * 1500.0 / 90000.0);
  CHECK(stats->bits_per_second == static_cast<double>(bytes) * 8.0 / stats->seconds);
  CHECK(stats->mean_qp == qp / 121.0);
  // Reversed bounds and out-of-range positions are normalised.
  CHECK(table.range_stats(240, 120)->bytes == bytes);
  CHECK(table.range_stats(299, 1000)->frames == 1);

  // The last frame of the stream takes the previous frame distance as its duration.
  CHECK(table.range_stats(299, 299)->ticks == 1500);
}

TEST_CASE("display order re-sorts the bars and keeps the selection on the same frame", "[app][timeline][order]") {
  // Decode order I P B B: pts 0 3 1 2.
  const std::vector<SyntheticFrame> frames{
      {0, 500, 20, true, 0}, {3, 80, 26, false, 0}, {1, 30, 30, false, 1}, {2, 31, 30, false, 1}};
  FrameTable table{StreamIndexHandle::adopt(build_index(frames, 1, 30))->index()};
  CHECK(table.order() == FrameOrder::Decode);
  CHECK(table.decode_index_at(1) == 1);
  const auto bits = [&](FrameOrder o) {
    table.set_order(o);
    return std::vector<double>(table.aggregator({Series::Bits, {}, 0}).values().begin(),
                               table.aggregator({Series::Bits, {}, 0}).values().end());
  };
  CHECK(bits(FrameOrder::Decode) == std::vector<double>{4000, 640, 240, 248});
  CHECK(bits(FrameOrder::Display) == std::vector<double>{4000, 240, 248, 640});

  // The frame at decode position 1 (the P frame) is at display position 3.
  table.set_order(FrameOrder::Decode);
  CHECK(table.set_order(FrameOrder::Display, 1) == std::optional<std::size_t>(3));
  CHECK(table.decode_index_at(3) == 1);
  CHECK(table.position_of(1) == std::optional<std::size_t>(3));
  CHECK(table.rap_positions() == std::vector<std::size_t>{0});
  // Range statistics are by frames of the positions in the current order: display 1..2 are the two B frames.
  CHECK(table.range_stats(1, 2)->bytes == 61);
}

TEST_CASE("series: qp, temporal id, bits and metrics with gaps", "[app][timeline][series]") {
  const std::vector<SyntheticFrame> frames{{0, 500, 20, true, 0}, {1, 80, 26, false, 2}, {2, 30, 30, false, 1}};
  FrameTable table{StreamIndexHandle::adopt(build_index(frames, 1, 30))->index()};
  CHECK(table.aggregator({Series::Qp, {}, 0}).max(0, 3) == 30.0);
  CHECK(table.aggregator({Series::TemporalId, {}, 0}).max(0, 3) == 2.0);
  CHECK(std::isnan(table.aggregator({Series::Metric, "psnr", 0}).max(0, 3)));  // never supplied
  table.set_metric("psnr", 0, {40.0, std::nan(""), 38.0});
  const auto& psnr = table.aggregator({Series::Metric, "psnr", 0});
  CHECK(psnr.max(0, 3) == 40.0);
  CHECK(psnr.mean(0, 3) == 39.0);
  CHECK(std::isnan(psnr.max(1, 2)));
  CHECK(std::isnan(table.aggregator({Series::Metric, "psnr", 1}).max(0, 3)));  // another plane
}

TEST_CASE("StreamIndexModel: range selection, order toggle and signals", "[app][timeline][model]") {
  ovt::app::testing::qt_app();
  const std::vector<SyntheticFrame> frames{
      {0, 500, 20, true, 0}, {3, 80, 26, false, 0}, {1, 30, 30, false, 1}, {2, 31, 30, false, 1}};
  StreamIndexModel model;
  CHECK(model.frame_count() == 0);
  CHECK(model.selectionIndex() == nullptr);
  model.setRange(0, 1);  // no index: ignored
  CHECK_FALSE(model.has_range());

  QSignalSpy index_spy(&model, &StreamIndexModel::indexChanged);
  QSignalSpy range_spy(&model, &StreamIndexModel::rangeChanged);
  QSignalSpy order_spy(&model, &StreamIndexModel::orderChanged);
  model.setIndex(StreamIndexHandle::adopt(build_index(frames, 1, 30)));
  CHECK(index_spy.count() == 1);
  CHECK(model.frame_count() == 4);

  model.setRange(3, 1);  // reversed
  REQUIRE(range_spy.count() == 1);
  CHECK(range_spy.at(0).at(0).toInt() == 1);
  CHECK(range_spy.at(0).at(1).toInt() == 3);
  model.setRange(1, 3);  // same range: no signal
  CHECK(range_spy.count() == 1);
  CHECK(model.range_stats_map().value(QStringLiteral("frames")).toInt() == 3);
  CHECK(model.range_stats_map().value(QStringLiteral("bytes")).toInt() == 141);

  // Decode positions 1..3 are frames P, B, B; in display order the P frame is last, so the range keeps the same first
  // and last frames (P and the second B): display positions 2..3.
  model.set_display_order(true);
  CHECK(order_spy.count() == 1);
  CHECK(model.display_order());
  CHECK(model.range_first() == 2);
  CHECK(model.range_last() == 3);

  model.setRange(100, 100);  // clamped
  CHECK(model.range_first() == 3);
  model.clearRange();
  CHECK_FALSE(model.has_range());
  CHECK(model.range_stats_map().isEmpty());
  CHECK(range_spy.back().at(0).toInt() == -1);

  model.setIndex(nullptr);
  CHECK(model.frame_count() == 0);
}
