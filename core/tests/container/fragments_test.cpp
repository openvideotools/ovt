// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the FragmentTimeline builder (chapter 08 §8.4.3, WI-1.219a), with the fragment samples checked against the
// MP4Box `-dts` goldens of the container-smoke corpus.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/fragments.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/cancel.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;

Bytes be(uint64_t v, unsigned nbytes) {
  Bytes b;
  for (unsigned i = nbytes; i > 0; --i) {
    b.push_back(static_cast<uint8_t>(v >> (8U * (i - 1U))));
  }
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  return cat({be(8 + payload.size(), 4), Bytes(type.begin(), type.end()), payload});
}
Bytes full(uint8_t version, uint32_t flags, const Bytes& body) {
  return cat({be(version, 1), be(flags, 3), body});
}

constexpr uint32_t kTrack = 1;
constexpr uint64_t kTimescale = 1000;
constexpr uint64_t kDur = 40;   // trex default_sample_duration
constexpr uint64_t kSize = 10;  // trex default_sample_size
constexpr uint32_t kNonSync = 0x10000;

Bytes ftyp() {
  return box("ftyp", cat({Bytes{'i', 's', 'o', '6'}, be(0, 4), Bytes{'i', 's', 'o', '6'}}));
}

Bytes trak(uint32_t id) {
  const Bytes tkhd = box("tkhd", full(0, 3, cat({be(0, 8), be(id, 4), be(0, 68)})));
  const Bytes mdhd = box("mdhd", full(0, 0, cat({be(0, 8), be(kTimescale, 4), be(0, 8)})));
  return box("trak", cat({tkhd, box("mdia", mdhd)}));
}

Bytes trex(uint32_t id) {
  return box("trex", full(0, 0, cat({be(id, 4), be(1, 4), be(kDur, 4), be(kSize, 4), be(kNonSync, 4)})));
}

Bytes moov(std::initializer_list<uint32_t> ids, bool with_trex = true) {
  Bytes traks;
  Bytes trexes;
  for (const uint32_t id : ids) {
    traks = cat({traks, trak(id)});
    trexes = cat({trexes, trex(id)});
  }
  return box("moov", cat({traks, with_trex ? box("mvex", trexes) : Bytes{}}));
}

// One traf: tfhd (flags; `extra` holds the optional tfhd fields in order), optional tfdt, one trun with `n` samples
// (first sample sync) and an optional data_offset; `trun_flags` adds per-sample fields (sizes are kSize).
struct TrafSpec {
  uint32_t track = kTrack;
  uint32_t tfhd_flags = 0x020000;  // default-base-is-moof
  Bytes tfhd_extra;
  std::optional<uint64_t> tfdt;
  uint32_t samples = 3;
  bool with_data_offset = true;
  int32_t data_offset = 0;    // filled by fragment() when 0 and with_data_offset (moof size + 8)
  uint32_t trun_flags = 0x5;  // data-offset + first-sample-flags
  Bytes extra_boxes;          // appended after the trun (saio, senc, ...)
};

Bytes traf(const TrafSpec& t, int32_t data_offset) {
  const Bytes tfhd = box("tfhd", full(0, t.tfhd_flags, cat({be(t.track, 4), t.tfhd_extra})));
  const Bytes tfdt = t.tfdt ? box("tfdt", full(1, 0, be(*t.tfdt, 8))) : Bytes{};
  uint32_t flags = t.trun_flags & ~(t.with_data_offset ? 0U : 1U);
  Bytes body = be(t.samples, 4);
  if ((flags & 1U) != 0) {
    body = cat({body, be(static_cast<uint32_t>(data_offset), 4)});
  }
  if ((flags & 4U) != 0) {
    body = cat({body, be(0, 4)});  // first sample: sync
  }
  for (uint32_t s = 0; s < t.samples && (flags & 0x200U) != 0; ++s) {
    body = cat({body, be(kSize, 4)});
  }
  return box("traf", cat({tfhd, tfdt, box("trun", full(0, flags, body)), t.extra_boxes}));
}

// A moof (mfhd `seq`, one traf per spec) and its mdat holding `mdat_bytes` bytes. A zero data_offset in a spec is
// replaced by the distance from the moof start to the mdat payload.
Bytes fragment(uint32_t seq, std::vector<TrafSpec> trafs, size_t mdat_bytes) {
  const auto build = [&](int32_t first) {
    Bytes body = box("mfhd", full(0, 0, be(seq, 4)));
    for (const TrafSpec& t : trafs) {
      body = cat({body, traf(t, t.data_offset != 0 ? t.data_offset : first)});
    }
    return box("moof", body);
  };
  const auto first = static_cast<int32_t>(build(0).size() + 8);
  return cat({build(first), box("mdat", Bytes(mdat_bytes, 0xAB))});
}

TrafSpec at(uint64_t tfdt) {
  TrafSpec t;
  t.tfdt = tfdt;
  return t;
}

struct Built {
  std::unique_ptr<ovt::io::MemoryFile> file;
  iso::WalkResult walked;
  iso::FragmentBuild b;
};

Built build(const Bytes& bytes, uint32_t track = kTrack, bool lazy = false) {
  Built r;
  r.file = std::make_unique<ovt::io::MemoryFile>(bytes);
  std::vector<iso::DeferredBox> deferred;
  r.walked = iso::walk(*r.file, lazy ? &deferred : nullptr);
  const iso::FragmentTrackInput in = iso::fragment_track_input(*r.file, r.walked.top_level, track);
  r.b = iso::build_fragments(*r.file, r.walked.top_level, in, ovt::jobs::Cancel::never());
  return r;
}

std::vector<std::string> codes(const iso::FragmentBuild& b) {
  std::vector<std::string> c;
  c.reserve(b.timeline.diagnostics.size());
  for (const auto& d : b.timeline.diagnostics) {
    c.push_back(d->code);
  }
  return c;
}
bool has(const iso::FragmentBuild& b, std::string_view code) {
  for (const auto& d : b.timeline.diagnostics) {
    if (d->code == code) {
      return true;
    }
  }
  return false;
}
std::string message(const iso::FragmentBuild& b, std::string_view code) {
  for (const auto& d : b.timeline.diagnostics) {
    if (d->code == code) {
      return d->message;
    }
  }
  return {};
}

Bytes stream(size_t fragments, uint64_t jump_at = SIZE_MAX, uint64_t jump = 0) {
  Bytes f = cat({ftyp(), moov({kTrack})});
  uint64_t t = 0;
  for (size_t k = 0; k < fragments; ++k) {
    t += k == jump_at ? jump : 0;
    f = cat({f, fragment(static_cast<uint32_t>(k + 1), {at(t)}, 3 * kSize)});
    t += 3 * kDur;
  }
  return f;
}

}  // namespace

TEST_CASE("fragments: offsets, sizes and DTS of a clean fragmented stream", "[container][fragments]") {
  const Built r = build(stream(4));
  const auto& tl = r.b.timeline;
  CHECK(codes(r.b).empty());
  REQUIRE(tl.fragments.size() == 4);
  REQUIRE(r.b.samples.count() == 12);
  CHECK(tl.track_id == kTrack);
  for (size_t k = 0; k < 4; ++k) {
    const auto& f = *tl.fragments[k];
    CHECK(f.index == k);
    CHECK(f.sequence_number == k + 1);
    CHECK(f.dts_gap == 0);
    CHECK(f.styp_offset == -1);
    CHECK(f.mdat_offset == f.moof_offset + f.moof_size);
    REQUIRE(f.trafs.size() == 1);
    const auto& t = *f.trafs[0];
    CHECK(t.base_data_offset == f.moof_offset);
    CHECK(t.default_sample_duration == kDur);
    CHECK(t.default_sample_size == kSize);
    CHECK(t.tfdt == 3 * kDur * k);
    REQUIRE(t.truns.size() == 1);
    CHECK(t.truns[0]->first_sample_offset == f.mdat_offset + 8);
    CHECK(t.truns[0]->duration_sum == 3 * kDur);
    CHECK(t.truns[0]->size_sum == 3 * kSize);
    for (size_t s = 0; s < 3; ++s) {
      const size_t i = 3 * k + s;
      CHECK(r.b.samples.offset[i] == f.mdat_offset + 8 + s * kSize);
      CHECK(r.b.samples.size[i] == kSize);
      CHECK(r.b.samples.dts[i] == static_cast<int64_t>(kDur * i));
      CHECK(r.b.samples.is_sync(i) == (s == 0));  // first_sample_flags 0, then trex flags (non-sync)
      CHECK(r.b.samples.fragment_index[i] == static_cast<int32_t>(k));
    }
  }
}

TEST_CASE("fragments: the serialised timeline is a valid, deterministic FlatBuffer", "[container][fragments]") {
  const Built a = build(stream(3));
  const Built b = build(stream(3));
  const auto x = iso::serialise(a.b.timeline);
  const auto y = iso::serialise(b.b.timeline);
  flatbuffers::Verifier v(x.data(), x.size());
  CHECK(v.VerifyBuffer<ovt::fb::FragmentTimeline>(nullptr));
  CHECK(std::vector<uint8_t>(x.data(), x.data() + x.size()) == std::vector<uint8_t>(y.data(), y.data() + y.size()));
}

TEST_CASE("fragments: a tfdt jump of 9000 before fragment 3 is CNT-4020 with that gap", "[container][fragments]") {
  // Acceptance test 4. Fragments are numbered from 0 (FragmentInfo.index); the message also names sequence_number.
  const Built r = build(stream(5, 3, 9000));
  REQUIRE(r.b.timeline.fragments.size() == 5);
  CHECK(r.b.timeline.fragments[3]->dts_gap == 9000);
  CHECK(r.b.timeline.fragments[3]->expected_dts == 9 * kDur);
  CHECK(r.b.timeline.fragments[4]->dts_gap == 0);
  REQUIRE(codes(r.b) == std::vector<std::string>{"CNT-4020"});
  CHECK(message(r.b, "CNT-4020").find("gap of 9000 ticks") != std::string::npos);
  CHECK(message(r.b, "CNT-4020").find("before fragment 3 ") != std::string::npos);
  const Built back = build(stream(3, 2, static_cast<uint64_t>(-60)));  // overlap
  CHECK(message(back.b, "CNT-4020").find("overlap of 60 ticks") != std::string::npos);
}

TEST_CASE("fragments: two trafs without base flags use the implicit base chain (CNT-4006)", "[container][fragments]") {
  // Acceptance test 3 (synthetic Smooth Streaming layout): the first traf's base is the moof, the second continues
  // after the first traf's data, so its data_offset is relative to that end.
  TrafSpec a = at(0);
  a.tfhd_flags = 0;
  a.samples = 2;
  TrafSpec b = at(0);
  b.track = 2;
  b.tfhd_flags = 0;
  b.samples = 4;
  b.data_offset = 6;  // 6 bytes of padding after track 1's data
  const Bytes bytes = cat({ftyp(), moov({1, 2}), fragment(1, {a, b}, 6 * kSize + 6)});
  const Built r1 = build(bytes, 1);
  const Built r2 = build(bytes, 2);
  CHECK(has(r1.b, "CNT-4006"));
  CHECK(has(r2.b, "CNT-4006"));
  CHECK_FALSE(has(r2.b, "CNT-4021"));
  CHECK_FALSE(has(r2.b, "CNT-4022"));
  const uint64_t payload = r1.b.timeline.fragments[0]->mdat_offset + 8;
  CHECK(r1.b.samples.offset == std::vector<uint64_t>{payload, payload + kSize});
  const auto& t2 = *r2.b.timeline.fragments[0]->trafs[0];
  CHECK(t2.base_data_offset == payload + 2 * kSize);
  REQUIRE(r2.b.samples.count() == 4);
  CHECK(r2.b.samples.offset[0] == payload + 2 * kSize + 6);
  CHECK(r2.b.samples.offset[3] == payload + 2 * kSize + 6 + 3 * kSize);
}

TEST_CASE("fragments: base_data_offset and data-offset-less truns", "[container][fragments]") {
  SECTION("explicit base_data_offset") {
    const Bytes probe = cat({ftyp(), moov({kTrack})});
    TrafSpec t = at(0);
    t.tfhd_flags = 0x1;
    t.tfhd_extra = be(0, 8);
    t.with_data_offset = false;
    // base 0 and no data_offset: the samples start at file offset 0 → outside the mdat.
    const Built r = build(cat({probe, fragment(1, {t}, 3 * kSize)}));
    CHECK(r.b.timeline.fragments[0]->trafs[0]->base_data_offset == 0);
    CHECK(r.b.samples.offset[0] == 0);
    CHECK(has(r.b, "CNT-4021"));
    CHECK_FALSE(has(r.b, "CNT-4007"));
  }
  SECTION("no data_offset and no base_data_offset") {
    TrafSpec t = at(0);
    t.with_data_offset = false;
    const Built r = build(cat({ftyp(), moov({kTrack}), fragment(1, {t}, 3 * kSize)}));
    CHECK(has(r.b, "CNT-4007"));
    CHECK(r.b.samples.offset[0] == r.b.timeline.fragments[0]->moof_offset);
  }
}

TEST_CASE("fragments: pairing, defaults, bounds and count diagnostics", "[container][fragments]") {
  const Bytes head = cat({ftyp(), moov({kTrack})});
  SECTION("moof without mdat (CNT-4004) and samples outside it (CNT-4021)") {
    const Bytes f = fragment(1, {at(0)}, 3 * kSize);
    const Bytes moof(f.begin(), f.end() - static_cast<long>(8 + 3 * kSize));
    const Built r = build(cat({head, moof, box("free", Bytes(4)), box("mdat", Bytes(3 * kSize))}));
    CHECK(has(r.b, "CNT-4004"));
    CHECK(has(r.b, "CNT-4021"));  // data_offset still points just past the moof, at the free box
  }
  SECTION("unreferenced mdat tail (CNT-4022)") {
    const Built r = build(cat({head, fragment(1, {at(0)}, 3 * kSize + 5)}));
    REQUIRE(codes(r.b) == std::vector<std::string>{"CNT-4022"});
    CHECK(message(r.b, "CNT-4022").starts_with("5 bytes"));
  }
  SECTION("no tfdt: DTS inferred (CNT-4008)") {
    TrafSpec t;
    const Built r = build(cat({head, fragment(1, {at(0)}, 3 * kSize), fragment(2, {t}, 3 * kSize)}));
    CHECK(has(r.b, "CNT-4008"));
    CHECK(r.b.samples.dts[3] == 3 * kDur);
    CHECK_FALSE(r.b.timeline.fragments[1]->trafs[0]->has_tfdt);
    CHECK(r.b.timeline.fragments[1]->trafs[0]->computed_dts_start == 3 * kDur);
  }
  SECTION("no trex and no tfhd defaults (CNT-4005)") {
    const Built r = build(cat({ftyp(), moov({kTrack}, false), fragment(1, {at(0)}, 0)}));
    CHECK(has(r.b, "CNT-4005"));
    CHECK(r.b.samples.duration[0] == 0);
  }
  SECTION("tfhd defaults win over trex") {
    TrafSpec t = at(0);
    t.tfhd_flags = 0x020000 | 0x8 | 0x10;
    t.tfhd_extra = cat({be(25, 4), be(4, 4)});
    const Built r = build(cat({head, fragment(1, {t}, 12)}));
    CHECK(codes(r.b).empty());
    CHECK(r.b.samples.duration == std::vector<uint32_t>{25, 25, 25});
    CHECK(r.b.samples.size == std::vector<uint32_t>{4, 4, 4});
  }
  SECTION("empty trun without duration-is-empty (CNT-4035)") {
    TrafSpec t = at(0);
    t.samples = 0;
    const Built r = build(cat({head, fragment(1, {t}, 0)}));
    CHECK(has(r.b, "CNT-4035"));
    t.tfhd_flags |= 0x10000;
    CHECK_FALSE(has(build(cat({head, fragment(1, {t}, 0)})).b, "CNT-4035"));
  }
  SECTION("sequence numbers not increasing (CNT-4001)") {
    const Built r = build(cat({head, fragment(2, {at(0)}, 3 * kSize), fragment(2, {at(3 * kDur)}, 3 * kSize)}));
    REQUIRE(codes(r.b) == std::vector<std::string>{"CNT-4001"});
  }
  SECTION("styp brands differ (CNT-4036)") {
    const Bytes s1 = box("styp", cat({Bytes{'m', 's', 'd', 'h'}, be(0, 4)}));
    const Bytes s2 = box("styp", cat({Bytes{'m', 's', 'i', 'x'}, be(0, 4)}));
    const Built r = build(cat({head, s1, fragment(1, {at(0)}, 3 * kSize), s2, fragment(2, {at(120)}, 3 * kSize)}));
    REQUIRE(codes(r.b) == std::vector<std::string>{"CNT-4036"});
    CHECK(r.b.timeline.fragments[0]->styp_offset == static_cast<int64_t>(head.size()));
  }
  SECTION("a track without fragments has an empty timeline") {
    const Built r = build(cat({head, fragment(1, {at(0)}, 3 * kSize)}), 7);
    CHECK(r.b.timeline.fragments.empty());
    CHECK(r.b.samples.count() == 0);
  }
}

TEST_CASE("fragments: the first fragment continues the moov samples", "[container][fragments]") {
  // A track with 2 moov samples of 40 ticks (stts + stsz): expected DTS of fragment 0 is 80.
  const Bytes stbl = box("stbl", cat({box("stts", full(0, 0, cat({be(1, 4), be(2, 4), be(40, 4)}))),
                                      box("stsz", full(0, 0, cat({be(kSize, 4), be(2, 4)})))}));
  const Bytes tkhd = box("tkhd", full(0, 3, cat({be(0, 8), be(kTrack, 4), be(0, 68)})));
  const Bytes mdhd = box("mdhd", full(0, 0, cat({be(0, 8), be(kTimescale, 4), be(0, 8)})));
  const Bytes mv = box(
      "moov", cat({box("trak", cat({tkhd, box("mdia", cat({mdhd, box("minf", stbl)}))})), box("mvex", trex(kTrack))}));
  const Built ok = build(cat({ftyp(), mv, fragment(1, {at(80)}, 3 * kSize)}));
  CHECK(codes(ok.b).empty());
  CHECK(ok.b.timeline.fragments[0]->expected_dts == 80);
  const Built gap = build(cat({ftyp(), mv, fragment(1, {at(0)}, 3 * kSize)}));
  CHECK(gap.b.timeline.fragments[0]->dts_gap == -80);
  CHECK(has(gap.b, "CNT-4020"));
}

TEST_CASE("fragments: saio resolution against senc (CNT-6005)", "[container][fragments]") {
  // senc v0 with 3 samples of 8-byte IVs; saio v0 with one offset relative to the moof (default-base-is-moof).
  const Bytes senc = box("senc", full(0, 0, cat({be(3, 4), Bytes(24, 0x11)})));
  const auto with_saio = [&](uint32_t off) {
    TrafSpec t = at(0);
    t.extra_boxes = cat({box("saio", full(0, 0, cat({be(1, 4), be(off, 4)}))), senc});
    return cat({ftyp(), moov({kTrack}), fragment(1, {t}, 3 * kSize)});
  };
  const Built probe = build(with_saio(0));
  const auto& t = *probe.b.timeline.fragments[0]->trafs[0];
  REQUIRE(t.senc_offset != 0);
  const uint64_t want = t.senc_offset + 16 - t.base_data_offset;  // box header + version/flags + sample_count
  const Built ok = build(with_saio(static_cast<uint32_t>(want)));
  CHECK_FALSE(has(ok.b, "CNT-6005"));
  CHECK(ok.b.timeline.fragments[0]->trafs[0]->saio_resolved == std::vector<uint64_t>{t.senc_offset + 16});
  CHECK(has(probe.b, "CNT-6005"));
}

TEST_CASE("fragments: header-only (lazy) moofs give the same timeline", "[container][fragments][lazy]") {
  // More than kEagerMoofLimit moofs: the lazy walk keeps only their mfhd, the builder reads the rest itself.
  const Bytes bytes = stream(iso::kEagerMoofLimit + 6);
  const Built eager = build(bytes);
  const Built lazy = build(bytes, kTrack, true);
  REQUIRE(lazy.b.samples.count() == 3 * (iso::kEagerMoofLimit + 6));
  CHECK(lazy.b.samples.offset == eager.b.samples.offset);
  CHECK(lazy.b.samples.dts == eager.b.samples.dts);
  const auto x = iso::serialise(eager.b.timeline);
  const auto y = iso::serialise(lazy.b.timeline);
  CHECK(std::vector<uint8_t>(x.data(), x.data() + x.size()) == std::vector<uint8_t>(y.data(), y.data() + y.size()));
}

TEST_CASE("fragments: a requested cancel stops the build", "[container][fragments]") {
  const Bytes bytes = stream(3);
  const ovt::io::MemoryFile file(bytes);
  const iso::WalkResult w = iso::walk(file);
  ovt::jobs::Cancel cancel;
  cancel.request();
  CHECK_THROWS_AS(iso::build_fragments(file, w.top_level, iso::fragment_track_input(file, w.top_level, kTrack), cancel),
                  ovt::Cancelled);
}

namespace {

// The track_ID of the only trak (decoded tkhd field).
uint32_t single_track_id(const iso::WalkResult& w) {
  const auto find = [](const auto& nodes, std::string_view type) -> const ovt::fb::BoxNodeT* {
    for (const auto& n : nodes) {
      if (n->fourcc == type) {
        return n.get();
      }
    }
    return nullptr;
  };
  const ovt::fb::BoxNodeT* moov = find(w.top_level, "moov");
  const ovt::fb::BoxNodeT* trak = moov != nullptr ? find(moov->children, "trak") : nullptr;
  const ovt::fb::BoxNodeT* tkhd = trak != nullptr ? find(trak->children, "tkhd") : nullptr;
  if (tkhd == nullptr) {
    return 0;
  }
  for (const auto& f : tkhd->fields) {
    if (f->name == "track_ID") {
      return static_cast<uint32_t>(f->uvalue);
    }
  }
  return 0;
}

// Compares the fragment samples of every track of `id` with its MP4Box `-dts` golden rows that follow the track's
// moov samples. Returns the number of rows compared.
size_t compare_dts_golden(std::string_view id) {
  const auto path = ovt::test::corpus_or_skip(id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult w = iso::walk(*file);
  std::ifstream gf(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/isobmff" /
                   (std::string(id) + ".mp4box-dts.json"));
  REQUIRE(gf.good());
  const auto golden = nlohmann::json::parse(gf);
  std::map<uint32_t, std::vector<const nlohmann::json*>> rows;
  for (const auto& r : golden) {
    // Single-track goldens have no "track" key.
    rows[r.contains("track") ? r.at("track").get<uint32_t>() : single_track_id(w)].push_back(&r);
  }
  size_t compared = 0;
  for (const auto& [track, list] : rows) {
    const iso::FragmentTrackInput in = iso::fragment_track_input(*file, w.top_level, track);
    const iso::FragmentBuild b = iso::build_fragments(*file, w.top_level, in, ovt::jobs::Cancel::never());
    const size_t moov_rows = list.size() - b.samples.count();
    INFO(std::string(id) + " track " + std::to_string(track));
    REQUIRE(b.samples.count() <= list.size());
    for (size_t i = 0; i < b.samples.count(); ++i) {
      const nlohmann::json& g = *list[moov_rows + i];
      INFO("fragment sample " + std::to_string(i));
      CHECK(b.samples.offset[i] == g.at("offset").get<uint64_t>());
      CHECK(b.samples.size[i] == g.at("size").get<uint32_t>());
      CHECK(b.samples.dts[i] == g.at("dts").get<int64_t>());
      CHECK(b.samples.dts[i] + b.samples.cts_offset[i] == g.at("cts").get<int64_t>());
      CHECK(b.samples.is_sync(i) == (g.at("sync").get<int>() != 0));
      ++compared;
    }
  }
  return compared;
}

}  // namespace

TEST_CASE("fragment samples match the MP4Box -dts goldens of the container-smoke corpus",
          "[container][fragments][golden][needs-corpus]") {
  for (const std::string_view id : {"mpegff_10_fragments", "mpegff_a4_tone_fragmented",
                                    "mpegff_fragment_random_access_2", "mpegff_video_cenc_roll_frag1s"}) {
    CHECK(compare_dts_golden(id) > 0);
  }
}

TEST_CASE("fragments: the sample bound truncates the expansion (CNT-2031)", "[container][fragments]") {
  // A 24-byte trun claims 10^9 default-sized samples; the expansion stops at the bound.
  TrafSpec t = at(0);
  t.samples = 1'000'000'000;
  const ovt::io::MemoryFile file(cat({ftyp(), moov({kTrack}), fragment(1, {t}, 3 * kSize)}));
  const iso::WalkResult w = iso::walk(file);
  iso::FragmentTrackInput in = iso::fragment_track_input(file, w.top_level, kTrack);
  in.max_samples = 1000;
  const iso::FragmentBuild b = iso::build_fragments(file, w.top_level, in, ovt::jobs::Cancel::never());
  CHECK(b.samples.count() == 1000);
  CHECK(has(b, "CNT-2031"));
  CHECK(has(b, "CNT-4021"));
}

TEST_CASE("fragments: another track's huge default-sized trun is not iterated", "[container][fragments]") {
  // Track 2's trun claims 0xFFFFFFFF samples without per-sample fields: its data end is computed, not looped.
  TrafSpec a = at(0);
  a.tfhd_flags = 0;
  TrafSpec b = at(0);
  b.track = 2;
  b.tfhd_flags = 0;
  b.samples = 0xFFFFFFFFU;
  const Built r = build(cat({ftyp(), moov({1, 2}), fragment(1, {a, b}, 3 * kSize)}), 1);
  REQUIRE(r.b.samples.count() == 3);
  CHECK(r.b.samples.offset[0] == r.b.timeline.fragments[0]->mdat_offset + 8);
  CHECK_FALSE(has(r.b, "CNT-4022"));  // the claimed run extends past the mdat end
}
