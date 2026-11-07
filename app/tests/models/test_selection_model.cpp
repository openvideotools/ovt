// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of SelectionModel (WI-1.452; chapter 11 §11.4.1): every derivation row of the "Who updates on what" table,
// the clear rules, the probe lock, level cycling and echo suppression on `origin`.
//
// Acceptance test 1 of the WI names the golden tests/golden/epb/syn_h264_epb_dense.json, which is produced by the
// oracle image (chapter 14 WI-0.5xx) and is not in the repository yet. SPEC-ASSUMPTION(#219): until it is, the EPB
// test below checks setField against an independent oracle built in the test: a reference emulation-prevention
// encoder that records where every RBSP byte lands in the escaped bytes, over RBSPs full of zero runs.

#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "models/selection_model.h"
#include "qt_test_app.h"

namespace {

using ovt::app::BitSpan;
using ovt::app::ByteSpan;
using ovt::app::FrameId;
using ovt::app::SelectionIndex;
using ovt::app::SelectionModel;
using ovt::app::SelectionSample;
using ovt::app::SelectionUnit;

// A StreamIndex stand-in: a vector of units, samples and frames.
class FakeIndex final : public SelectionIndex {
 public:
  struct Unit {
    ByteSpan span;
    std::vector<std::uint32_t> epb;
    FrameId frame;
    int sample = -1;
  };
  std::vector<Unit> units;
  std::map<int, SelectionSample> samples;
  std::vector<FrameId> frames;  // both indices filled

  [[nodiscard]] std::optional<SelectionUnit> unit(int i) const override {
    if (i < 0 || static_cast<std::size_t>(i) >= units.size()) {
      return std::nullopt;
    }
    const Unit& u = units[static_cast<std::size_t>(i)];
    return SelectionUnit{u.span, u.epb, u.frame, u.sample};
  }
  [[nodiscard]] std::optional<FrameId> resolve_frame(FrameId f) const override {
    for (const FrameId& known : frames) {
      if (known.decode_index == f.decode_index || (f.decode_index < 0 && known.display_index == f.display_index)) {
        return known;
      }
    }
    return std::nullopt;
  }
  [[nodiscard]] int first_vcl_unit(FrameId f) const override {
    for (std::size_t i = 0; i < units.size(); ++i) {
      if (units[i].frame.decode_index >= 0 && units[i].frame.decode_index == f.decode_index) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  [[nodiscard]] std::optional<SelectionSample> sample(int i) const override {
    const auto it = samples.find(i);
    return it == samples.end() ? std::nullopt : std::optional<SelectionSample>(it->second);
  }
  [[nodiscard]] int unit_at_byte(std::uint64_t offset) const override {
    for (std::size_t i = 0; i < units.size(); ++i) {
      if (offset >= units[i].span.offset && offset < units[i].span.offset + units[i].span.length) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
};

// Units: 0 SPS (non-VCL, sample 0), 1 slice of frame decode 0 / display 1 (sample 0), 2 slice of frame decode 1 /
// display 0 (sample 1), 3 SEI (non-VCL, sample 1).
std::shared_ptr<FakeIndex> make_index() {
  auto index = std::make_shared<FakeIndex>();
  index->units = {
      {{100, 20}, {}, {-1, -1}, 0},
      {{124, 50}, {}, {0, 1}, 0},
      {{178, 40}, {}, {1, 0}, 1},
      {{222, 10}, {}, {-1, -1}, 1},
  };
  index->frames = {{0, 1}, {1, 0}};
  index->samples[0] = SelectionSample{{96, 78}, {0, 1}, 0};
  index->samples[1] = SelectionSample{{178, 54}, {1, 0}, 2};
  return index;
}

struct Fixture {
  SelectionModel model;
  std::vector<int> masks;
  Fixture() {
    ovt::app::testing::qt_app();
    model.setIndex(make_index());
    QObject::connect(&model, &SelectionModel::changed, &model, [this](int mask) { masks.push_back(mask); });
  }
};

constexpr int bit(SelectionModel::Change c) {
  return static_cast<int>(c);
}

}  // namespace

TEST_CASE("SelectionModel: setFrame fills the unit, sample and bytes", "[fast][gui][selection]") {
  Fixture f;
  f.model.setFrame(FrameId{1, -1}, QStringLiteral("timeline"));
  CHECK(f.model.frame() == FrameId{1, 0});  // both indices filled from the index
  CHECK(f.model.unit_index() == 2);         // first VCL unit of the frame
  CHECK(f.model.sample_index() == 1);
  CHECK(f.model.byte_range() == ByteSpan{178, 40});
  REQUIRE(f.masks.size() == 1);
  CHECK(f.masks[0] == (bit(SelectionModel::ChangeFrame) | bit(SelectionModel::ChangeUnit) |
                       bit(SelectionModel::ChangeSample) | bit(SelectionModel::ChangeBytes)));
  CHECK(f.model.origin() == QStringLiteral("timeline"));

  f.model.setFrame(FrameId{-1, 1}, QStringLiteral("timeline"));  // by display index
  CHECK(f.model.frame() == FrameId{0, 1});
  CHECK(f.model.unit_index() == 1);

  f.masks.clear();
  f.model.setFrame(FrameId{99, -1}, QStringLiteral("timeline"));  // not in the stream: ignored
  CHECK(f.model.frame() == FrameId{0, 1});
  CHECK(f.masks.empty());
}

TEST_CASE("SelectionModel: a new frame resets block, mv and probe unless the pixel selection is locked",
          "[fast][gui][selection]") {
  Fixture f;
  f.model.setFrame(FrameId{0, -1}, QStringLiteral("a"));
  f.model.setBlock(7, 0, QStringLiteral("picture"));
  f.model.setMv(3, QStringLiteral("picture"));
  f.model.setProbe(QPoint(10, 20), QStringLiteral("picture"));
  f.model.setRect(QRect(1, 2, 3, 4), QStringLiteral("picture"));
  REQUIRE(f.model.mv_index() == 3);  // setMv after setBlock

  SECTION("lock off") {
    f.masks.clear();
    f.model.setFrame(FrameId{1, -1}, QStringLiteral("timeline"));
    CHECK(f.model.block_id() == SelectionModel::kNoBlock);
    CHECK(f.model.level() == -1);
    CHECK(f.model.mv_index() == -1);
    CHECK(f.model.probe() == QPoint(-1, -1));
    CHECK(f.model.selection_rect() == QRect(1, 2, 3, 4));
    REQUIRE(f.masks.size() == 1);
    CHECK((f.masks[0] & bit(SelectionModel::ChangeBlock)) != 0);
    CHECK((f.masks[0] & bit(SelectionModel::ChangeMv)) != 0);
    CHECK((f.masks[0] & bit(SelectionModel::ChangeProbe)) != 0);
  }
  SECTION("lock on keeps the probe") {
    f.model.set_lock_pixel_selection(true);
    f.model.setFrame(FrameId{1, -1}, QStringLiteral("timeline"));
    CHECK(f.model.block_id() == SelectionModel::kNoBlock);
    CHECK(f.model.mv_index() == -1);
    CHECK(f.model.probe() == QPoint(10, 20));
  }
  SECTION("the same frame again changes nothing") {
    f.masks.clear();
    f.model.setFrame(FrameId{0, -1}, QStringLiteral("timeline"));
    CHECK(f.model.block_id() == 7);
    CHECK(f.model.probe() == QPoint(10, 20));
    CHECK(f.masks.empty());
  }
}

TEST_CASE("SelectionModel: setUnit derives frame, sample and bytes", "[fast][gui][selection]") {
  Fixture f;
  f.model.setUnit(2, QStringLiteral("units"));  // VCL
  CHECK(f.model.frame() == FrameId{1, 0});
  CHECK(f.model.sample_index() == 1);
  CHECK(f.model.byte_range() == ByteSpan{178, 40});
  CHECK(f.model.unit_index() == 2);

  f.model.setUnit(0, QStringLiteral("units"));  // non-VCL: the frame stays
  CHECK(f.model.frame() == FrameId{1, 0});
  CHECK(f.model.sample_index() == 0);
  CHECK(f.model.byte_range() == ByteSpan{100, 20});

  f.masks.clear();
  f.model.setUnit(42, QStringLiteral("units"));  // no such unit: ignored
  CHECK(f.model.unit_index() == 0);
  CHECK(f.masks.empty());
}

TEST_CASE("SelectionModel: setSample derives bytes, frame and unit", "[fast][gui][selection]") {
  Fixture f;
  f.model.setSample(1, QStringLiteral("container"));
  CHECK(f.model.byte_range() == ByteSpan{178, 54});
  CHECK(f.model.frame() == FrameId{1, 0});
  CHECK(f.model.unit_index() == 2);
  CHECK(f.model.sample_index() == 1);

  f.model.setSample(9, QStringLiteral("container"));  // unknown sample: ignored
  CHECK(f.model.sample_index() == 1);
}

TEST_CASE("SelectionModel: setBytes finds the containing unit", "[fast][gui][selection]") {
  Fixture f;
  f.model.setBytes(ByteSpan{130, 4}, QStringLiteral("hex"));
  CHECK(f.model.unit_index() == 1);
  CHECK(f.model.frame() == FrameId{0, 1});
  CHECK(f.model.byte_range() == ByteSpan{130, 4});
  CHECK(f.model.field_path().isEmpty());

  f.model.setBytes(ByteSpan{500, 4}, QStringLiteral("hex"));  // outside every unit
  CHECK(f.model.unit_index() == -1);
  CHECK(f.model.byte_range() == ByteSpan{500, 4});
}

TEST_CASE("SelectionModel: RBSP to raw mapping handles 0, 1, many and inner EPBs", "[fast][gui][selection]") {
  auto unit = [](std::vector<std::uint32_t> epb) { return std::make_pair(ByteSpan{1000, 64}, std::move(epb)); };
  const auto map = [](const std::pair<ByteSpan, std::vector<std::uint32_t>>& u, std::uint64_t bit_offset) {
    return SelectionModel::raw_offset_of_bit(SelectionUnit{u.first, u.second, {0, 0}, -1}, bit_offset);
  };
  SECTION("no EPB") {
    const auto u = unit({});
    CHECK(map(u, 0) == 1000);
    CHECK(map(u, 37) == 1004);
  }
  SECTION("one EPB at raw offset 3") {
    const auto u = unit({3});
    CHECK(map(u, 2 * 8) == 1002);  // before the EPB
    CHECK(map(u, 3 * 8) == 1004);  // the rbsp byte that follows the EPB lands one later
    CHECK(map(u, 3 * 8 + 5) == 1004);
  }
  SECTION("many EPBs") {
    const auto u = unit({3, 7, 11});
    CHECK(map(u, 5 * 8) == 1006);      // rbsp byte 5: raw 5 -> 6 (EPB at 3 passed)
    CHECK(map(u, 6 * 8) == 1008);      // raw 6 -> 7 (3) -> 8 (7)
    CHECK(map(u, 9 * 8 + 1) == 1012);  // raw 9 -> 10 (3) -> 11 (7) -> 12 (11); bit within the byte unchanged
  }
}

namespace {

// Reference emulation-prevention encoder: escapes an RBSP and returns, for each RBSP byte, its offset in the escaped
// bytes, plus the offsets of the inserted 0x03 bytes (what StreamIndex stores as epb_positions).
struct Escaped {
  std::vector<std::uint32_t> epb;
  std::vector<std::uint32_t> position_of;  // per RBSP byte
};

Escaped escape(const std::vector<std::uint8_t>& rbsp) {
  Escaped out;
  std::uint32_t raw = 0;
  int zeros = 0;
  for (const std::uint8_t byte : rbsp) {
    if (zeros >= 2 && byte <= 3) {
      out.epb.push_back(raw++);
      zeros = 0;
    }
    out.position_of.push_back(raw++);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  return out;
}

}  // namespace

TEST_CASE("SelectionModel: setField maps every field to the oracle byte offsets (EPB inside fields)",
          "[fast][gui][selection][epb]") {
  // An RBSP full of zero runs, as syn_h264_epb_dense produces: EPBs fall inside multi-byte fields.
  const std::vector<std::uint8_t> rbsp = {0x67, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x80, 0x00, 0x00,
                                          0x03, 0xFF, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  const Escaped escaped = escape(rbsp);
  REQUIRE(escaped.epb.size() >= 5);

  auto index = std::make_shared<FakeIndex>();
  index->units = {{{5000, rbsp.size() + escaped.epb.size()}, escaped.epb, {0, 0}, 0}};
  index->frames = {{0, 0}};
  Fixture f;
  f.model.setIndex(index);

  int checked = 0;
  // Every contiguous field of 1..40 bits at every start bit of the RBSP.
  for (std::uint64_t start = 0; start < rbsp.size() * 8; start += 3) {
    for (std::uint32_t length = 1; length <= 40 && start + length <= rbsp.size() * 8; length += 7) {
      const std::uint64_t first_byte = start / 8;
      const std::uint64_t last_byte = (start + length - 1) / 8;
      const std::uint64_t expect_offset = 5000 + escaped.position_of[first_byte];
      const std::uint64_t expect_end = 5000 + escaped.position_of[last_byte];
      f.model.setField(0, QStringLiteral("f/%1_%2").arg(start).arg(length), BitSpan{start, length},
                       QStringLiteral("syn"));
      REQUIRE(f.model.byte_range().offset == expect_offset);
      REQUIRE(f.model.byte_range().offset + f.model.byte_range().length - 1 == expect_end);
      REQUIRE(f.model.bit_range() == BitSpan{start, length});
      ++checked;
    }
  }
  CHECK(checked > 100);
  CHECK(f.model.unit_index() == 0);
  CHECK(f.model.frame() == FrameId{0, 0});
}

TEST_CASE("SelectionModel: setBlock cycles the level chain", "[fast][gui][selection]") {
  Fixture f;
  f.model.setChainLength([](std::uint32_t) { return 3; });  // e.g. HEVC CTU, CU, PU
  f.model.setFrame(FrameId{0, -1}, QStringLiteral("picture"));
  f.model.setMv(2, QStringLiteral("picture"));

  f.model.setBlock(5, 0, QStringLiteral("picture"));
  CHECK(f.model.level() == 0);
  CHECK(f.model.mv_index() == -1);  // setting a block clears the mv
  f.model.setBlock(5, -1, QStringLiteral("picture"));
  CHECK(f.model.level() == 1);
  f.model.setBlock(5, -1, QStringLiteral("picture"));
  CHECK(f.model.level() == 2);
  f.model.setBlock(5, -1, QStringLiteral("picture"));
  CHECK(f.model.level() == 0);  // wraps after the chain length

  f.model.setBlock(6, -1, QStringLiteral("picture"));  // a different leaf starts at the coarsest level
  CHECK(f.model.block_id() == 6);
  CHECK(f.model.level() == 0);
  f.model.setBlock(6, 9, QStringLiteral("picture"));  // explicit level is clamped to the chain
  CHECK(f.model.level() == 2);

  f.model.clearBlock(QStringLiteral("picture"));
  CHECK(f.model.block_id() == SelectionModel::kNoBlock);
  CHECK(f.model.level() == -1);
}

TEST_CASE("SelectionModel: echo suppression keys on origin", "[fast][gui][selection]") {
  Fixture f;
  bool timeline_saw_echo = false;
  bool hex_saw_echo = false;
  QObject::connect(&f.model, &SelectionModel::changed, &f.model, [&](int) {
    timeline_saw_echo = f.model.isEcho(QStringLiteral("timeline"));
    hex_saw_echo = f.model.isEcho(QStringLiteral("hex"));
  });
  f.model.setFrame(FrameId{0, -1}, QStringLiteral("timeline"));
  CHECK(timeline_saw_echo);
  CHECK_FALSE(hex_saw_echo);

  // Same value from another panel is still a change of origin only when the value differs; equal values emit nothing.
  f.masks.clear();
  f.model.setFrame(FrameId{0, -1}, QStringLiteral("hex"));
  CHECK(f.masks.empty());
  CHECK(f.model.origin() == QStringLiteral("timeline"));
  f.model.setProbe(QPoint(1, 1), QStringLiteral("hex"));
  CHECK(hex_saw_echo);
  CHECK_FALSE(timeline_saw_echo);
  CHECK_FALSE(f.model.isEcho(QString()));
}

TEST_CASE("SelectionModel: order, probe, rect and clear", "[fast][gui][selection]") {
  Fixture f;
  f.model.setDisplayOrder(false, QStringLiteral("timeline"));
  REQUIRE(f.masks.size() == 1);
  CHECK(f.masks[0] == bit(SelectionModel::ChangeOrder));
  f.model.setProbe(QPoint(4, 5), QStringLiteral("picture"));
  CHECK(f.masks.back() == bit(SelectionModel::ChangeProbe));
  f.model.setRect(QRect(0, 0, 8, 8), QStringLiteral("picture"));
  CHECK(f.masks.back() == bit(SelectionModel::ChangeRect));

  f.model.setSource(QStringLiteral("f0"), 1, QStringLiteral("project"));
  f.model.setFrame(FrameId{0, -1}, QStringLiteral("project"));
  f.model.clear(QStringLiteral("esc"));
  CHECK(f.model.frame() == FrameId{-1, -1});
  CHECK(f.model.unit_index() == -1);
  CHECK(f.model.byte_range().length == 0);
  CHECK(f.model.probe() == QPoint(-1, -1));
  CHECK(f.model.file_id() == QStringLiteral("f0"));  // file, track and order survive
  CHECK(f.model.track_index() == 1);
  CHECK_FALSE(f.model.display_order());
}

TEST_CASE("SelectionModel: without an index the setters store what they are given", "[fast][gui][selection]") {
  ovt::app::testing::qt_app();
  SelectionModel model;
  model.setFrame(FrameId{4, 2}, QStringLiteral("a"));
  CHECK(model.frame() == FrameId{4, 2});
  CHECK(model.unit_index() == -1);
  model.setUnit(3, QStringLiteral("a"));
  CHECK(model.unit_index() == 3);
  model.setField(3, QStringLiteral("x"), BitSpan{8, 8}, QStringLiteral("a"));
  CHECK(model.field_path() == QStringLiteral("x"));
  CHECK(model.byte_range().length == 0);
}
