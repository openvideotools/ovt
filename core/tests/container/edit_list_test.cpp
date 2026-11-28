// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the EditList and the elst ↔ tfdt sanity check (chapter 08 §8.4.2, WI-1.220) over synthetic MP4 files: the
// presentation timeline of a B-frame delay edit, an empty edit and the implicit edit, the sample range resolution
// through the CTS order, every CNT-30xx finding, the 128-bit presentation time and the corpus acceptance tests.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/edit_list.h"
#include "container/isobmff/fragments.h"
#include "container/isobmff/sample_table.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace iso = ovt::container::isobmff;

namespace {

using Bytes = std::vector<uint8_t>;

void put(Bytes& b, uint64_t v, int nbytes) {
  for (int s = (nbytes - 1) * 8; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
void put_all(Bytes& b, const Bytes& more) {
  b.insert(b.end(), more.begin(), more.end());
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    put_all(b, p);
  }
  return b;
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put(b, 8 + payload.size(), 4);
  b.insert(b.end(), type.begin(), type.end());
  put_all(b, payload);
  return b;
}
Bytes full_box(std::string_view type, uint32_t version_flags, const Bytes& payload) {
  Bytes p;
  put(p, version_flags, 4);
  put_all(p, payload);
  return box(type, p);
}
Bytes table(std::string_view type, uint32_t version_flags, const std::vector<std::vector<uint32_t>>& rows) {
  Bytes p;
  put(p, rows.size(), 4);
  for (const auto& r : rows) {
    for (const uint32_t v : r) {
      put(p, v, 4);
    }
  }
  return full_box(type, version_flags, p);
}

Bytes mvhd(uint32_t timescale, uint32_t duration) {
  Bytes p;
  put(p, 0, 8);
  put(p, timescale, 4);
  put(p, duration, 4);
  p.insert(p.end(), 80 + 6 + 24 + 4, 0);
  return full_box("mvhd", 0, p);
}
Bytes tkhd(uint32_t id, uint32_t duration) {
  Bytes p;
  put(p, 0, 8);
  put(p, id, 4);
  put(p, 0, 4);
  put(p, duration, 4);
  p.insert(p.end(), 8 + 8 + 36 + 8, 0);
  return full_box("tkhd", 3, p);
}
Bytes mdhd(uint32_t timescale, uint32_t duration) {
  Bytes p;
  put(p, 0, 8);
  put(p, timescale, 4);
  put(p, duration, 4);
  put(p, 0x55C4, 2);
  put(p, 0, 2);
  return full_box("mdhd", 0, p);
}
Bytes stsd() {
  Bytes p;
  put(p, 1, 4);
  put_all(p, box("avc1", Bytes(78, 0)));
  return full_box("stsd", 0, p);
}
Bytes stsz(const std::vector<uint32_t>& sizes) {
  Bytes p;
  put(p, 0, 4);
  put(p, sizes.size(), 4);
  for (const uint32_t s : sizes) {
    put(p, s, 4);
  }
  return full_box("stsz", 0, p);
}

struct Edit {
  uint64_t duration;
  int64_t media_time;
  uint16_t rate_integer = 1;
  uint16_t rate_fraction = 0;
};
Bytes elst(const std::vector<Edit>& edits, bool wide = false) {
  Bytes p;
  put(p, edits.size(), 4);
  for (const Edit& e : edits) {
    put(p, e.duration, wide ? 8 : 4);
    put(p, static_cast<uint64_t>(e.media_time), wide ? 8 : 4);
    put(p, e.rate_integer, 2);
    put(p, e.rate_fraction, 2);
  }
  return full_box("elst", wide ? 0x01000000U : 0U, p);
}

// One track: `samples` samples of `delta` ticks with the given composition offsets (ctts v0), `edits` (no edts when
// empty and `with_edts` is false), movie timescale `movie_ts`, track timescale `track_ts`.
struct Spec {
  uint32_t movie_ts = 1000;
  uint32_t track_ts = 1000;
  uint32_t tkhd_duration = 0;
  uint32_t mdhd_duration = 0;
  uint32_t delta = 40;
  std::vector<uint32_t> cts_offsets;
  std::optional<Bytes> elst_box;
};

Bytes mp4(const Spec& s) {
  const size_t n = s.cts_offsets.size();
  std::vector<std::vector<uint32_t>> ctts;
  ctts.reserve(n);
  for (const uint32_t o : s.cts_offsets) {
    ctts.push_back({1, o});
  }
  Bytes stbl = stsd();
  put_all(stbl, table("stts", 0, {{static_cast<uint32_t>(n), s.delta}}));
  put_all(stbl, table("ctts", 0, ctts));
  put_all(stbl, stsz(std::vector<uint32_t>(n, 10)));
  put_all(stbl, table("stsc", 0, {{1, static_cast<uint32_t>(n), 1}}));
  Bytes co;
  put(co, 1, 4);
  put(co, 0x1000, 4);
  put_all(stbl, full_box("stco", 0, co));
  Bytes trak_children = tkhd(1, s.tkhd_duration);
  if (s.elst_box) {
    put_all(trak_children, box("edts", *s.elst_box));
  }
  put_all(trak_children, box("mdia", cat({mdhd(s.track_ts, s.mdhd_duration), box("minf", box("stbl", stbl))})));
  Bytes ftyp;
  ftyp.insert(ftyp.end(), {'i', 's', 'o', 'm'});
  put(ftyp, 0, 4);
  return cat({box("ftyp", ftyp), box("moov", cat({mvhd(s.movie_ts, s.tkhd_duration), box("trak", trak_children)}))});
}

struct Built {
  Bytes data;
  std::unique_ptr<ovt::fb::SampleTableT> table;
  iso::EditListBuild build;
  [[nodiscard]] const ovt::fb::EditListT& list() const { return *build.list; }
  [[nodiscard]] bool has(std::string_view code) const {
    return std::ranges::any_of(list().diagnostics, [&](const auto& d) { return d->code == code; });
  }
  [[nodiscard]] std::optional<int64_t> pts(uint64_t i, uint32_t movie_ts) const {
    int64_t v = 0;
    return iso::sample_presentation_time(list(), *table, i, movie_ts, &v) ? std::optional<int64_t>(v) : std::nullopt;
  }
};

Built build_from(Bytes data) {
  Built b;
  b.data = std::move(data);
  const ovt::io::MemoryFile file(b.data);
  const iso::WalkResult w = iso::walk(file);
  const ovt::fb::BoxNodeT* trak = iso::find_trak(w.top_level, 0);
  REQUIRE(trak != nullptr);
  b.table = iso::expand_sample_table(file, *trak, ovt::jobs::Cancel::never());
  b.build = iso::build_edit_list(file, w.top_level, 0, *b.table, ovt::jobs::Cancel::never());
  return b;
}

// Six samples of 40 ticks in decode order with CTS 80, 200, 120, 160, 280, 240 (the display order is 0 2 3 1 5 4).
const std::vector<uint32_t> kDelay = {80, 160, 40, 40, 120, 40};

}  // namespace

TEST_CASE("edit list: a B-frame delay removed by elst is one normal segment (x264 default)",
          "[container][editlist][fast]") {
  // elst: media_time == the composition delay of sample 0; tkhd.duration is the edited duration (240 - 80).
  Spec s;
  s.tkhd_duration = 160;
  s.mdhd_duration = 240;
  s.cts_offsets = kDelay;
  s.elst_box = elst({{160, 80}});
  const Built b = build_from(mp4(s));
  const auto& l = b.list();
  REQUIRE(l.segments.size() == 1);
  const auto& seg = *l.segments[0];
  CHECK(seg.kind == "normal");
  CHECK(seg.media_time == b.table->dts[0] + b.table->cts_offset[0]);  // CTS[0] - DTS[0] with DTS[0] == 0
  CHECK(seg.presentation_start == 0);
  CHECK(seg.presentation_end == 160);
  CHECK(seg.media_rate == 1.0);
  CHECK(seg.first_sample == 0);  // CTS 80
  CHECK(seg.last_sample == 1);   // the largest CTS below 80 + 160 is 200 (sample 1)
  CHECK(l.implied_start_offset == 80);
  CHECK(l.negative_cts_compensation);
  CHECK(l.total_presentation_duration == 160);
  CHECK(l.track_id == 1);
  CHECK(l.diagnostics.empty());
  CHECK(b.build.has_elst);
  // Presentation order, not decode order: the first presented frame has PT 0.
  CHECK(b.pts(0, 1000) == 0);
  CHECK(b.pts(2, 1000) == 40);
  CHECK(b.pts(3, 1000) == 80);
  CHECK(b.pts(1, 1000) == 120);
  CHECK_FALSE(b.pts(5, 1000));  // CTS 240 is past the edit
  CHECK_FALSE(b.pts(4, 1000));
  CHECK_FALSE(b.pts(6, 1000));  // no such sample
}

TEST_CASE("edit list: no elst is one implicit normal segment (§8.4.2 step 1)", "[container][editlist][fast]") {
  Spec s;
  s.tkhd_duration = 0;  // unknown: taken from mdhd
  s.mdhd_duration = 240;
  s.movie_ts = 500;
  s.track_ts = 1000;
  s.cts_offsets = kDelay;
  const Built b = build_from(mp4(s));
  const auto& l = b.list();
  REQUIRE(l.segments.size() == 1);
  CHECK_FALSE(b.build.has_elst);
  CHECK(l.segments[0]->kind == "normal");
  CHECK(l.segments[0]->media_time == 0);
  CHECK(l.segments[0]->segment_duration == 120);  // 240 track ticks in the movie timescale
  CHECK(l.implied_start_offset == 0);
  CHECK_FALSE(l.negative_cts_compensation);  // 0 = none (SPEC-ASSUMPTION #793)
  CHECK(l.diagnostics.empty());
  // PT = CTS rescaled; every sample is presented.
  CHECK(b.pts(0, 500) == 40);
  CHECK(b.pts(1, 500) == 100);
  CHECK(b.pts(4, 500) == 140);
}

TEST_CASE("edit list: a QuickTime empty edit delays the media by its duration", "[container][editlist][fast]") {
  Spec s;
  s.movie_ts = 1000;
  s.track_ts = 2000;
  s.delta = 1000;  // 500 ms per sample
  s.tkhd_duration = 1500;
  s.mdhd_duration = 3000;
  s.cts_offsets = {0, 0, 0};
  s.elst_box = elst({{500, -1}, {1000, 0}});
  const Built b = build_from(mp4(s));
  const auto& l = b.list();
  REQUIRE(l.segments.size() == 2);
  CHECK(l.segments[0]->kind == "empty");
  CHECK(l.segments[0]->first_sample == -1);
  CHECK(l.segments[0]->last_sample == -1);
  CHECK(l.segments[0]->presentation_start == 0);
  CHECK(l.segments[0]->presentation_end == 500);
  CHECK(l.segments[1]->kind == "normal");
  CHECK(l.segments[1]->presentation_start == 500);
  CHECK(l.segments[1]->presentation_end == 1500);
  CHECK(l.segments[1]->first_sample == 0);
  CHECK(l.segments[1]->last_sample == 1);  // CTS 0 and 1000 are below 2000; CTS 2000 is not
  CHECK(l.implied_start_offset == 0);
  CHECK(l.total_presentation_duration == 1500);
  CHECK(l.diagnostics.empty());
  CHECK(b.pts(0, 1000) == 500);  // the empty edit duration
  CHECK(b.pts(1, 1000) == 1000);
  CHECK_FALSE(b.pts(2, 1000));
}

TEST_CASE("edit list: version 1 entries and a truncated entry table", "[container][editlist][fast]") {
  Spec s;
  s.tkhd_duration = 160;
  s.mdhd_duration = 240;
  s.cts_offsets = kDelay;
  s.elst_box = elst({{160, 80}}, true);
  const Built wide = build_from(mp4(s));
  REQUIRE(wide.list().segments.size() == 1);
  CHECK(wide.list().segments[0]->media_time == 80);
  CHECK(wide.list().diagnostics.empty());

  // entry_count claims 1,000,000 entries, the payload holds one: the entries that fit are kept.
  Bytes p;
  put(p, 1000000, 4);
  put(p, 160, 4);
  put(p, 80, 4);
  put(p, 1, 2);
  put(p, 0, 2);
  s.elst_box = full_box("elst", 0, p);
  const Built cut = build_from(mp4(s));
  CHECK(cut.list().segments.size() == 1);
}

TEST_CASE("edit list: CNT-3001, CNT-3002 and CNT-3004", "[container][editlist][fast]") {
  Spec s;
  s.tkhd_duration = 160;
  s.mdhd_duration = 240;
  s.cts_offsets = kDelay;
  SECTION("starts beyond the media") {
    s.elst_box = elst({{160, 320}});  // the composition end is 320
    CHECK(build_from(mp4(s)).has("CNT-3001"));
  }
  SECTION("exceeds the media") {
    s.elst_box = elst({{400, 80}});  // ends at 480 > 320
    const Built b = build_from(mp4(s));
    CHECK(b.has("CNT-3002"));
    CHECK(b.has("CNT-3004"));  // 200 != tkhd.duration 160
    CHECK_FALSE(b.has("CNT-3001"));
  }
  SECTION("less than one movie tick of rounding is not a finding") {
    s.movie_ts = 700;  // one movie tick = 2 track ticks (rounded up)
    s.tkhd_duration = 169;
    s.elst_box = elst({{169, 80}});  // ends at 80 + 241 = 321, 1 over 320
    CHECK_FALSE(build_from(mp4(s)).has("CNT-3002"));
    s.tkhd_duration = 170;
    s.elst_box = elst({{170, 80}});  // 322, 2 over
    CHECK(build_from(mp4(s)).has("CNT-3002"));
  }
  SECTION("sum of the edits differs from tkhd.duration") {
    s.elst_box = elst({{100, 80}, {40, 80}});
    CHECK(build_from(mp4(s)).has("CNT-3004"));
  }
  SECTION("a delay-removing edit that keeps the full duration is clean (ffmpeg)") {
    s.tkhd_duration = 240;
    s.elst_box = elst({{240, 80}});  // ends at 320, the composition end
    const Built b = build_from(mp4(s));
    CHECK_FALSE(b.has("CNT-3002"));
    CHECK_FALSE(b.has("CNT-3001"));
  }
  SECTION("a matching sum is clean") {
    s.elst_box = elst({{100, 80}, {60, 80}});
    CHECK_FALSE(build_from(mp4(s)).has("CNT-3004"));
  }
}

TEST_CASE("edit list: rate and dwell edits", "[container][editlist][fast]") {
  Spec s;
  s.tkhd_duration = 100;
  s.mdhd_duration = 240;
  s.cts_offsets = kDelay;
  s.elst_box = elst({{40, 80, 2, 0}, {30, 160, 0, 0}, {30, 80}});
  const Built b = build_from(mp4(s));
  const auto& l = b.list();
  REQUIRE(l.segments.size() == 3);
  CHECK(b.has("CNT-3003"));
  CHECK(l.segments[0]->kind == "rate");
  CHECK(l.segments[0]->media_rate == 2.0);
  CHECK(l.segments[0]->first_sample == -1);  // informational only
  CHECK(l.segments[1]->kind == "dwell");
  CHECK(l.segments[1]->first_sample == 3);  // CTS 160
  CHECK(l.segments[1]->last_sample == 3);
  CHECK(l.segments[2]->kind == "normal");
  CHECK(l.implied_start_offset == 80);  // the first normal segment, not the rate one
  CHECK(b.pts(3, 1000) == 40);          // the frozen frame at the dwell's presentation start
  CHECK(b.pts(0, 1000) == 70);          // normal segment starts at 70 with media_time 80
}

TEST_CASE("edit list: the presentation time uses a 128-bit intermediate", "[container][editlist][fast]") {
  ovt::fb::SampleTableT t;
  t.timescale = 90000;
  t.count = 1;
  t.dts = {int64_t{1} << 60};
  t.cts_offset = {0};
  ovt::fb::EditListT l;
  auto seg = std::make_unique<ovt::fb::EditSegmentT>();
  seg->kind = "normal";
  seg->media_time = int64_t{1} << 60;
  seg->first_sample = 0;
  seg->last_sample = 0;
  seg->presentation_start = 7;
  l.segments.push_back(std::move(seg));
  int64_t pts = 0;
  REQUIRE(iso::sample_presentation_time(l, t, 0, 1000000, &pts));
  CHECK(pts == 7);
  // (2^62 - 2^60) * 1e6 overflows 64 bits but the result fits.
  t.dts = {int64_t{1} << 62};
  l.segments[0]->first_sample = 0;
  l.segments[0]->last_sample = 0;
  t.timescale = 1000000000;
  REQUIRE(iso::sample_presentation_time(l, t, 0, 1000, &pts));
  CHECK(pts == 7 + ((int64_t{1} << 62) - (int64_t{1} << 60)) / 1000000);
  // A result outside int64 is "not presented".
  t.timescale = 1;
  CHECK_FALSE(iso::sample_presentation_time(l, t, 0, 1000000, &pts));
  CHECK_FALSE(iso::sample_presentation_time(l, t, 0, 0, &pts));
  CHECK_FALSE(iso::sample_presentation_time(l, t, 0, 1000, nullptr));
}

TEST_CASE("edit list: the FlatBuffer round-trips", "[container][editlist][fast]") {
  Spec s;
  s.tkhd_duration = 160;
  s.mdhd_duration = 240;
  s.cts_offsets = kDelay;
  s.elst_box = elst({{160, 80}});
  const Built b = build_from(mp4(s));
  const flatbuffers::DetachedBuffer buf = iso::serialise(b.list());
  flatbuffers::Verifier v(buf.data(), buf.size());
  REQUIRE(v.VerifyBuffer<ovt::fb::EditList>(nullptr));
  const auto* root = flatbuffers::GetRoot<ovt::fb::EditList>(buf.data());
  CHECK(root->track_id() == 1);
  CHECK(root->segments()->size() == 1);
  CHECK(root->negative_cts_compensation());
}

namespace {

// A fragmented track whose first fragment starts at `t0` with composition offset `c0` and sample duration `d0`.
iso::FragmentBuild fragments(uint64_t t0, int32_t c0, uint32_t d0, bool has_tfdt = true) {
  iso::FragmentBuild f;
  auto info = std::make_unique<ovt::fb::FragmentInfoT>();
  auto traf = std::make_unique<ovt::fb::TrafInfoT>();
  traf->has_tfdt = has_tfdt;
  traf->tfdt = has_tfdt ? t0 : 0;
  traf->computed_dts_start = has_tfdt ? 0 : t0;
  info->trafs.push_back(std::move(traf));
  f.timeline.fragments.push_back(std::move(info));
  f.samples.dts = {static_cast<int64_t>(t0)};
  f.samples.cts_offset = {c0};
  f.samples.duration = {d0};
  return f;
}

iso::EditListBuild list_with(const std::vector<std::pair<std::string, int64_t>>& segments, bool has_elst = true) {
  iso::EditListBuild b;
  b.list = std::make_unique<ovt::fb::EditListT>();
  b.has_elst = has_elst;
  b.diag_offset = 40;
  for (const auto& [kind, media_time] : segments) {
    auto s = std::make_unique<ovt::fb::EditSegmentT>();
    s->kind = kind;
    s->media_time = media_time;
    if (kind == "normal" && b.list->implied_start_offset == 0) {
      b.list->implied_start_offset = media_time;
    }
    b.list->segments.push_back(std::move(s));
  }
  return b;
}

bool has(const iso::EditListBuild& b, std::string_view code) {
  return std::ranges::any_of(b.list->diagnostics, [&](const auto& d) { return d->code == code; });
}

}  // namespace

TEST_CASE("elst ↔ tfdt: CNT-3010 when media_time matches neither T0+C0 nor C0", "[container][editlist][fast]") {
  const iso::FragmentBuild f = fragments(90000, 2000, 1000);  // T0 + C0 = 92000
  SECTION("media_time == T0 + C0") {
    iso::EditListBuild b = list_with({{"normal", 92000}});
    iso::check_elst_tfdt(b, f);
    CHECK(b.list->diagnostics.empty());
  }
  SECTION("media_time == C0") {
    iso::EditListBuild b = list_with({{"normal", 2000}});
    iso::check_elst_tfdt(b, f);
    CHECK(b.list->diagnostics.empty());
  }
  SECTION("within one sample duration of T0 + C0") {
    iso::EditListBuild b = list_with({{"normal", 92500}});
    iso::check_elst_tfdt(b, f);
    CHECK(b.list->diagnostics.empty());
  }
  SECTION("media_time ≠ tfdt + cts") {
    iso::EditListBuild b = list_with({{"normal", 50000}});
    iso::check_elst_tfdt(b, f);
    REQUIRE(has(b, "CNT-3010"));
    CHECK(b.list->diagnostics[0]->span->bit_offset() == uint64_t{40} * 8U);
  }
  SECTION("an inferred start (no tfdt) counts as T0") {
    iso::EditListBuild b = list_with({{"normal", 92000}});
    iso::check_elst_tfdt(b, fragments(90000, 2000, 1000, false));
    CHECK(b.list->diagnostics.empty());
  }
}

TEST_CASE("elst ↔ tfdt: CNT-3011 and CNT-3012", "[container][editlist][fast]") {
  SECTION("no elst, T0 > 0 and C0 > 0") {
    iso::EditListBuild b = list_with({{"normal", 0}}, false);
    iso::check_elst_tfdt(b, fragments(90000, 2000, 1000));
    CHECK(has(b, "CNT-3011"));
    CHECK_FALSE(has(b, "CNT-3010"));
  }
  SECTION("no elst and T0 == 0 or C0 == 0 is quiet") {
    iso::EditListBuild a = list_with({{"normal", 0}}, false);
    iso::check_elst_tfdt(a, fragments(0, 2000, 1000));
    CHECK(a.list->diagnostics.empty());
    iso::EditListBuild b = list_with({{"normal", 0}}, false);
    iso::check_elst_tfdt(b, fragments(90000, 0, 1000));
    CHECK(b.list->diagnostics.empty());
  }
  SECTION("two normal segments in a fragmented file") {
    iso::EditListBuild b = list_with({{"empty", -1}, {"normal", 0}, {"normal", 500}});
    iso::check_elst_tfdt(b, fragments(0, 0, 1000));
    CHECK(has(b, "CNT-3012"));
  }
  SECTION("a track without fragments is not checked") {
    iso::EditListBuild b = list_with({{"normal", 1}, {"normal", 500}});
    iso::check_elst_tfdt(b, iso::FragmentBuild{});
    CHECK(b.list->diagnostics.empty());
  }
}

TEST_CASE("edit list: the corpus files of the acceptance tests", "[container][editlist][golden][needs-corpus]") {
  SECTION("syn_h264_weightedpred_ltr: the elst removes the B-frame delay") {
    // The entry also holds the .264 elementary stream: open the MP4 member explicitly.
    auto path = ovt::test::corpus_or_skip("syn_h264_weightedpred_ltr");
    path = path.parent_path() / "syn_h264_weightedpred_ltr.mp4";
    REQUIRE(std::filesystem::exists(path));
    const auto file = ovt::io::RandomAccessFile::open(path.string());
    const iso::WalkResult w = iso::walk(*file);
    const ovt::fb::BoxNodeT* trak = iso::find_trak(w.top_level, 0);
    REQUIRE(trak != nullptr);
    const auto table = iso::expand_sample_table(*file, *trak, ovt::jobs::Cancel::never());
    REQUIRE(table->count > 0);
    const auto b = iso::build_edit_list(*file, w.top_level, 0, *table, ovt::jobs::Cancel::never());
    const auto& normals = b.list->segments;
    REQUIRE(normals.size() == 1);
    CHECK(normals[0]->kind == "normal");
    CHECK(normals[0]->media_time == table->cts_offset[0]);
    CHECK(normals[0]->media_time > 0);
    CHECK(b.list->negative_cts_compensation);
    for (const auto& d : b.list->diagnostics) {
      INFO(d->code + ": " + d->message);
      CHECK(d->severity != ovt::fb::Severity::Error);
    }
  }
  SECTION("syn_mp4_elst: a 500 ms empty edit") {
    const auto path = ovt::test::corpus_or_skip("syn_mp4_edit_list_frag_cenc");
    const auto file = ovt::io::RandomAccessFile::open(path.string());
    const iso::WalkResult w = iso::walk(*file);
    const ovt::fb::BoxNodeT* trak = iso::find_trak(w.top_level, 0);
    REQUIRE(trak != nullptr);
    const auto table = iso::expand_sample_table(*file, *trak, ovt::jobs::Cancel::never());
    const auto b = iso::build_edit_list(*file, w.top_level, 0, *table, ovt::jobs::Cancel::never());
    REQUIRE(b.list->segments.size() == 2);
    for (const auto& d : b.list->diagnostics) {
      INFO(d->code + ": " + d->message);
      CHECK(d->severity != ovt::fb::Severity::Error);
    }
    CHECK(b.list->segments[0]->kind == "empty");
    CHECK(b.list->segments[1]->kind == "normal");
    int64_t pts = -1;
    REQUIRE(iso::sample_presentation_time(*b.list, *table, 0, b.movie_timescale, &pts));
    // The first normal edit starts at the first sample's CTS, so the sample is presented where the empty edit ends.
    REQUIRE(b.list->segments[1]->media_time == table->dts[0] + table->cts_offset[0]);
    CHECK(pts == static_cast<int64_t>(b.list->segments[0]->segment_duration));
  }
}
