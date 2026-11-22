// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the FragmentTimeline builder (chapter 08 §8.4.3, WI-1.219a/b), with the fragment samples checked against the
// MP4Box `-dts` goldens of the container-smoke corpus.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/fragments.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/cancel.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/jobs.hpp"
#include "ovt/jobs.h"
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

namespace {

// A sidx (v0, reference_ID kTrack) in front of `frags`, one media reference per fragment.
Bytes sidx_for(const std::vector<Bytes>& frags, int32_t size_delta, int32_t dur_delta, bool sap) {
  Bytes refs;
  for (const Bytes& f : frags) {
    refs = cat({refs, be(static_cast<uint32_t>(static_cast<int64_t>(f.size()) + size_delta), 4),
                be(static_cast<uint64_t>(int64_t{3 * kDur} + dur_delta), 4), be(sap ? 0x90000000U : 0U, 4)});
  }
  return box(
      "sidx",
      full(0, 0, cat({be(kTrack, 4), be(kTimescale, 4), be(0, 4), be(0, 4), be(0, 2), be(frags.size(), 2), refs})));
}

}  // namespace

TEST_CASE("fragments: sidx references match the fragments they cover", "[container][fragments][sidx]") {
  std::vector<Bytes> frags;
  frags.reserve(3);
  for (uint32_t k = 0; k < 3; ++k) {
    frags.push_back(fragment(k + 1, {at(3 * kDur * k)}, 3 * kSize));
  }
  const Bytes head = cat({ftyp(), moov({kTrack})});
  const auto file = [&](const Bytes& sidx) { return cat({head, sidx, frags[0], frags[1], frags[2]}); };
  SECTION("consistent sidx: no CNT-4030 / CNT-4032 and actual_size == referenced_size") {
    const Built r = build(file(sidx_for(frags, 0, 0, true)));
    CHECK(codes(r.b).empty());
    REQUIRE(r.b.timeline.sidx.size() == 1);
    const auto& s = *r.b.timeline.sidx[0];
    REQUIRE(s.refs.size() == 3);
    for (size_t k = 0; k < 3; ++k) {
      CHECK(s.refs[k]->actual_size == static_cast<int64_t>(s.refs[k]->referenced_size));
      CHECK(s.refs[k]->actual_duration == static_cast<int64_t>(s.refs[k]->subsegment_duration));
      CHECK(s.refs[k]->abs_offset == r.b.timeline.fragments[k]->moof_offset);
      CHECK(r.b.timeline.fragments[k]->sidx_reference_index == static_cast<int32_t>(k));
    }
  }
  SECTION("wrong sizes and durations") {
    const Built r = build(file(sidx_for(frags, 4, 10, false)));
    CHECK(has(r.b, "CNT-4030"));
    CHECK(has(r.b, "CNT-4032"));
  }
  SECTION("SAP claimed on a non-sync first sample (CNT-4034)") {
    std::vector<Bytes> ns;
    TrafSpec t = at(0);
    t.trun_flags = 0x1;  // no first_sample_flags: trex flags say non-sync
    ns.push_back(fragment(1, {t}, 3 * kSize));
    const Built r = build(cat({head, sidx_for(ns, 0, 0, true), ns[0]}));
    REQUIRE(codes(r.b) == std::vector<std::string>{"CNT-4034"});
  }
}

TEST_CASE("fragments: many sidx references over many moofs stay near-linear", "[container][fragments][sidx]") {
  // 40 sidx boxes × 4000 references over 4000 moofs: the old linear scans did ~10⁹ steps here.
  constexpr size_t kFrags = 4000;
  constexpr size_t kSidx = 40;
  std::vector<Bytes> frags;
  frags.reserve(kFrags);
  for (size_t k = 0; k < kFrags; ++k) {
    frags.push_back(fragment(static_cast<uint32_t>(k + 1), {at(3 * kDur * k)}, 3 * kSize));
  }
  Bytes refs;
  for (const Bytes& f : frags) {
    for (const Bytes& field : {be(f.size(), 4), be(3 * kDur, 4), be(0x90000000U, 4)}) {
      refs.insert(refs.end(), field.begin(), field.end());
    }
  }
  Bytes bytes = cat({ftyp(), moov({kTrack})});
  for (size_t i = 0; i < kSidx; ++i) {
    // Each sidx skips the sidx boxes after it (first_offset; a v0 sidx is 32 bytes + references), so all of them
    // reference the same fragments.
    const uint64_t skip = (kSidx - 1 - i) * (32 + refs.size());
    const Bytes s =
        box("sidx",
            full(0, 0, cat({be(kTrack, 4), be(kTimescale, 4), be(0, 4), be(skip, 4), be(0, 2), be(kFrags, 2), refs})));
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  for (const Bytes& f : frags) {
    bytes.insert(bytes.end(), f.begin(), f.end());
  }
  const auto t0 = std::chrono::steady_clock::now();
  const Built r = build(bytes);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(codes(r.b).empty());
  REQUIRE(r.b.timeline.sidx.size() == kSidx);
  REQUIRE(r.b.timeline.fragments.size() == kFrags);
  // The last sidx in file order wins the fragment's reference index.
  CHECK(r.b.timeline.fragments[0]->sidx_reference_index == static_cast<int32_t>((kSidx - 1) * kFrags));
  CHECK(r.b.timeline.fragments[kFrags - 1]->sidx_reference_index == static_cast<int32_t>(kSidx * kFrags - 1));
  CHECK(elapsed < std::chrono::seconds(10));  // the quadratic version took ~24 s in a debug build, this one < 1 s
}

TEST_CASE("fragments: a sidx whose offsets overflow is CNT-4030", "[container][fragments][sidx]") {
  const Bytes head = cat({ftyp(), moov({kTrack})});
  const Bytes f0 = fragment(1, {at(0)}, 3 * kSize);
  const Bytes refs = cat({be(f0.size(), 4), be(3 * kDur, 4), be(0, 4), be(16, 4), be(3 * kDur, 4), be(0, 4)});
  const Bytes s = box(
      "sidx",
      full(1, 0, cat({be(kTrack, 4), be(kTimescale, 4), be(0, 8), be(UINT64_MAX - 100, 8), be(0, 2), be(2, 2), refs})));
  const Built r = build(cat({head, s, f0}));
  REQUIRE(r.b.timeline.sidx.size() == 1);
  const auto& info = *r.b.timeline.sidx[0];
  REQUIRE(info.refs.size() == 2);
  CHECK(info.refs[0]->actual_size == -1);
  CHECK(info.refs[1]->actual_size == -1);
  CHECK(message(r.b, "CNT-4030").find("overflows") != std::string::npos);
  CHECK(r.b.timeline.fragments[0]->sidx_reference_index == -1);
}

TEST_CASE("fragments: mfra / tfra / mfro cross-checks", "[container][fragments][mfra]") {
  const Bytes head = cat({ftyp(), moov({kTrack})});
  const Bytes f0 = fragment(1, {at(0)}, 3 * kSize);
  const Bytes f1 = fragment(2, {at(120)}, 3 * kSize);
  const auto mfra = [&](uint64_t moof1, uint64_t time1, int32_t mfro_delta) {
    const Bytes tfra = box("tfra", full(0, 0,
                                        cat({be(kTrack, 4), be(0, 4), be(2, 4),             //
                                             be(0, 4), be(head.size(), 4), Bytes{1, 1, 1},  //
                                             be(time1, 4), be(moof1, 4), Bytes{1, 1, 1}})));
    const uint64_t size = 8 + tfra.size() + 16;
    return box("mfra", cat({tfra, box("mfro", full(0, 0, be(size + static_cast<uint64_t>(mfro_delta), 4)))}));
  };
  const uint64_t moof1 = head.size() + f0.size();
  CHECK(codes(build(cat({head, f0, f1, mfra(moof1, 120, 0)})).b).empty());
  const Built bad = build(cat({head, f0, f1, mfra(moof1 + 1, 120, 0)}));
  CHECK(codes(bad.b) == std::vector<std::string>{"CNT-4010"});
  CHECK(has(build(cat({head, f0, f1, mfra(moof1, 121, 0)})).b, "CNT-4010"));
  CHECK(has(build(cat({head, f0, f1, mfra(moof1, 120, 4)})).b, "CNT-4011"));
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
  in.max_samples = 100;
  const iso::FragmentBuild b = iso::build_fragments(file, w.top_level, in, ovt::jobs::Cancel::never());
  CHECK(b.samples.count() == 100);
  CHECK(has(b, "CNT-2031"));
  CHECK(has(b, "CNT-4021"));
  // Without a lower max_samples, the file size bounds the expansion (one byte per sample, #788).
  const iso::FragmentBuild f = iso::build_fragments(
      file, w.top_level, iso::fragment_track_input(file, w.top_level, kTrack), ovt::jobs::Cancel::never());
  CHECK(f.samples.count() == file.size());
  CHECK(message(f, "CNT-2031").find("in a file of") != std::string::npos);
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

TEST_CASE("dashif_sidx_m4s: every sidx reference matches its fragments",
          "[container][fragments][sidx][slow][needs-corpus]") {
  // Acceptance test 2.
  const auto path = ovt::test::corpus_or_skip("dashif_sidx_m4s");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult w = iso::walk(*file, nullptr);
  const iso::FragmentTrackInput in = iso::fragment_track_input(*file, w.top_level, 1);
  const iso::FragmentBuild b = iso::build_fragments(*file, w.top_level, in, ovt::jobs::Cancel::never());
  CHECK_FALSE(has(b, "CNT-4030"));
  CHECK_FALSE(has(b, "CNT-4032"));
  REQUIRE_FALSE(b.timeline.sidx.empty());
  for (const auto& s : b.timeline.sidx) {
    for (const auto& r : s->refs) {
      CHECK(r->actual_size == static_cast<int64_t>(r->referenced_size));
    }
  }
}

// ---- WI-1.219c: the SampleTable fragment part, TrackInfo.sample_count and ovt_container_fragments ----

namespace {

// An open container over `bytes` (closed by the destructor).
struct Opened {
  ovt_File handle;
  ovt_Container* c = nullptr;
  explicit Opened(const Bytes& bytes) : handle{std::make_unique<ovt::io::MemoryFile>(bytes)} {
    REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  }
  explicit Opened(const std::filesystem::path& path) : handle{ovt::io::RandomAccessFile::open(path.string())} {
    REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  Opened(Opened&&) = delete;
  Opened& operator=(Opened&&) = delete;
  ~Opened() { ovt_container_close(c); }
};

// Takes ownership of a caller-owned ovt_Buffer (§12.3.1 rule 3a).
struct Owned {
  ovt_Buffer b{};
  Owned() = default;
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  Owned(Owned&&) = delete;
  Owned& operator=(Owned&&) = delete;
  ~Owned() { delete[] b.data; }
};

std::unique_ptr<ovt::fb::SampleTableT> table_of(ovt_Container* c, int track) {
  Owned o;
  REQUIRE(ovt_container_sample_table(c, track, 0, 0, &o.b) == OVT_OK);
  flatbuffers::Verifier v(o.b.data, o.b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::SampleTable>(nullptr));
  return std::unique_ptr<ovt::fb::SampleTableT>(flatbuffers::GetRoot<ovt::fb::SampleTable>(o.b.data)->UnPack());
}

}  // namespace

TEST_CASE("fragments: the SampleTable appends the fragment samples", "[container][fragments][sample_table][c_api]") {
  const Bytes seg = fragment(2, {at(120)}, 3 * kSize);
  const Opened o(cat({ftyp(), moov({kTrack}), fragment(1, {at(0)}, 3 * kSize), seg}));
  const auto table = table_of(o.c, 0);
  REQUIRE(table->count == 6);
  CHECK(table->dts == std::vector<int64_t>{0, 40, 80, 120, 160, 200});
  CHECK(table->fragment_index == std::vector<int32_t>{0, 0, 0, 1, 1, 1});
  CHECK(table->chunk_index == std::vector<uint32_t>(6, 0));
  CHECK(table->is_sync == std::vector<bool>{true, false, false, true, false, false});
  CHECK(table->sample_description_index == std::vector<uint16_t>(6, 1));
  CHECK(table->sdtp.size() == 6);
  CHECK(table->is_partial_sync.empty());
  CHECK(table->offset[3] == table->offset[2] + kSize + (seg.size() - 3 * kSize));
  Owned ti;
  REQUIRE(ovt_container_track_info(o.c, 0, &ti.b) == OVT_OK);
  CHECK(flatbuffers::GetRoot<ovt::fb::TrackInfo>(ti.b.data)->sample_count() == 6);  // Σ trun.sample_count (#768)
}

TEST_CASE("fragments: the fragment part continues the moov samples of the SampleTable",
          "[container][fragments][sample_table][c_api]") {
  const Bytes stbl = box("stbl", cat({box("stts", full(0, 0, cat({be(1, 4), be(2, 4), be(40, 4)}))),
                                      box("stsz", full(0, 0, cat({be(kSize, 4), be(2, 4)})))}));
  const Bytes tkhd = box("tkhd", full(0, 3, cat({be(0, 8), be(kTrack, 4), be(0, 68)})));
  const Bytes mdhd = box("mdhd", full(0, 0, cat({be(0, 8), be(kTimescale, 4), be(0, 8)})));
  const Bytes mv = box(
      "moov", cat({box("trak", cat({tkhd, box("mdia", cat({mdhd, box("minf", stbl)}))})), box("mvex", trex(kTrack))}));
  const Opened o(cat({ftyp(), mv, fragment(1, {at(80)}, 3 * kSize)}));
  const auto table = table_of(o.c, 0);
  REQUIRE(table->count == 5);
  CHECK(table->dts == std::vector<int64_t>{0, 40, 80, 120, 160});
  CHECK(table->fragment_index == std::vector<int32_t>{-1, -1, 0, 0, 0});
  Owned tl;
  REQUIRE(ovt_container_fragments(o.c, 0, nullptr, &tl.b) == OVT_OK);
  const auto* timeline = flatbuffers::GetRoot<ovt::fb::FragmentTimeline>(tl.b.data);
  REQUIRE(timeline->fragments()->size() == 1);
  CHECK(timeline->fragments()->Get(0)->expected_dts() == 80);
  CHECK(timeline->fragments()->Get(0)->dts_gap() == 0);
  Owned ti;
  REQUIRE(ovt_container_track_info(o.c, 0, &ti.b) == OVT_OK);
  CHECK(flatbuffers::GetRoot<ovt::fb::TrackInfo>(ti.b.data)->sample_count() == 5);  // stsz 2 + trun 3 (#768)
}

TEST_CASE("fragments: append_fragment_samples packs sample_flags and pads moov-only columns",
          "[container][fragments][sample_table]") {
  ovt::fb::SampleTableT table;
  table.count = 1;
  table.dts = {0};
  table.cts_offset = {0};
  table.duration = {10};
  table.offset = {100};
  table.size = {5};
  table.is_sync = {true};
  table.is_partial_sync = {false};  // stps present in the moov part
  table.sample_description_index = {1};
  table.chunk_index = {1};
  table.fragment_index = {-1};
  table.groups.push_back(std::make_unique<ovt::fb::GroupMembershipT>());
  table.groups.back()->description_index = {3};
  ovt::fb::FragmentTimelineT timeline;
  iso::FragmentSamples s;
  s.dts = {10, 20};
  s.cts_offset = {0, -20};
  s.duration = {10, 10};
  s.offset = {200, 205};
  s.size = {5, 5};
  // sample_depends_on 2, sync, priority 0 / is_leading 1, sample_is_depended_on 2, non-sync, priority 0x1234.
  s.flags = {0x02000000U, 0x04810000U | 0x1234U};
  s.sample_description_index = {1, 70000};
  s.fragment_index = {0, 0};
  iso::append_fragment_samples(table, timeline, std::move(s));
  CHECK(table.count == 3);
  CHECK(table.sdtp == std::vector<uint8_t>{0, 0x20, 0x48});
  CHECK(table.degradation_priority == std::vector<uint16_t>{0, 0, 0x1234});
  CHECK(table.is_sync == std::vector<bool>{true, true, false});
  CHECK(table.is_partial_sync == std::vector<bool>{false, false, false});
  CHECK(table.sample_description_index == std::vector<uint16_t>{1, 1, 0xFFFF});
  CHECK(table.chunk_index == std::vector<uint32_t>{1, 0, 0});
  CHECK(table.fragment_index == std::vector<int32_t>{-1, 0, 0});
  CHECK(table.groups.back()->description_index == std::vector<uint32_t>{3, 0, 0});
  CHECK(table.composition_to_dts_shift == 20);
}

TEST_CASE("fragments: ovt_container_fragments arguments and cancellation", "[container][fragments][c_api]") {
  const Opened o(stream(3));
  Owned b;
  CHECK(ovt_container_fragments(o.c, 1, nullptr, &b.b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_fragments(o.c, -1, nullptr, &b.b) == OVT_E_INVALID_ARG);
  ovt_Job* job = nullptr;
  REQUIRE(ovt_job_create(&job) == OVT_OK);
  ovt_job_cancel(job);
  CHECK(ovt_container_fragments(o.c, 0, job, &b.b) == OVT_E_CANCELLED);
  CHECK(b.b.data == nullptr);
  ovt_job_reset(job);
  REQUIRE(ovt_container_fragments(o.c, 0, job, &b.b) == OVT_OK);  // the cancelled call kept nothing
  ovt_job_free(job);
  flatbuffers::Verifier v(b.b.data, b.b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::FragmentTimeline>(nullptr));
  const auto* t = flatbuffers::GetRoot<ovt::fb::FragmentTimeline>(b.b.data);
  CHECK(t->track_id() == kTrack);
  CHECK(t->fragments()->size() == 3);
}

TEST_CASE("fragments: the C API SampleTable equals every MP4Box -dts row, fragments included",
          "[container][fragments][sample_table][golden][needs-corpus]") {
  for (const std::string_view id : {"mpegff_10_fragments", "mpegff_a4_tone_fragmented",
                                    "mpegff_fragment_random_access_2", "mpegff_video_cenc_roll_frag1s"}) {
    const auto path = ovt::test::corpus_or_skip(id);
    std::ifstream gf(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/isobmff" /
                     (std::string(id) + ".mp4box-dts.json"));
    const auto golden = nlohmann::json::parse(gf);
    const Opened o(path);
    for (int k = 0; k < ovt_container_track_count(o.c); ++k) {
      const auto table = table_of(o.c, k);
      std::vector<const nlohmann::json*> rows;
      for (const auto& r : golden) {
        if (!r.contains("track") || r.at("track").get<uint32_t>() == table->track_id) {
          rows.push_back(&r);
        }
      }
      INFO(std::string(id) + " track " + std::to_string(table->track_id));
      REQUIRE(table->count == rows.size());
      for (uint64_t i = 0; i < table->count; ++i) {
        const nlohmann::json& g = *rows[i];
        INFO("sample " << i);
        CHECK(table->offset[i] == g.at("offset").get<uint64_t>());
        CHECK(table->size[i] == g.at("size").get<uint32_t>());
        CHECK(table->dts[i] == g.at("dts").get<int64_t>());
        CHECK(table->dts[i] + table->cts_offset[i] == g.at("cts").get<int64_t>());
        CHECK(table->is_sync[i] == (g.at("sync").get<int>() != 0));
      }
      Owned ti;
      REQUIRE(ovt_container_track_info(o.c, k, &ti.b) == OVT_OK);
      CHECK(flatbuffers::GetRoot<ovt::fb::TrackInfo>(ti.b.data)->sample_count() == rows.size());
    }
  }
}

namespace {

// Output of a shell command; empty when it cannot run. Test-only: every command is fixed text plus temp/corpus paths.
std::string run(const std::string& cmd) {
  // NOLINTNEXTLINE(cert-env33-c): test-only oracle call
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  std::string out;
  if (!pipe) {
    return out;
  }
  std::array<char, 4096> chunk{};
  while (fgets(chunk.data(), static_cast<int>(chunk.size()), pipe.get()) != nullptr) {
    out += chunk.data();
  }
  return out;
}

bool on_path(std::string_view tool) {
  return !run("command -v " + std::string(tool) + " 2>/dev/null").empty();
}

// An ffprobe JSON value: numbers are strings for some fields (pos, size) and numbers for others (dts).
int64_t num(const nlohmann::json& v) {
  return v.is_string() ? std::stoll(v.get<std::string>()) : v.get<int64_t>();
}

std::string quoted(const std::filesystem::path& p) {
  return "'" + p.string() + "'";
}

// Acceptance test 1 against one file: the first 1000 packets of the first video track equal ffprobe `pos`/`size`
// and its DTS (relative to the first packet: ffprobe shifts DTS by the edit list, the SampleTable holds media
// times, §8.9.2 item 4), and every fragment has dts_gap 0.
// SPEC-ASSUMPTION(#791): a fragment without `tfdt` takes its DTS from continuity (§8.4.3 step 2.4, CNT-4008), as
// `MP4Box -dts` does; libavformat derives a different one (sidx timing), so those samples are checked for
// continuity instead of against ffprobe. Returns {packets compared, of which DTS against ffprobe}.
std::pair<size_t, size_t> compare_with_ffprobe(const std::filesystem::path& path) {
  const auto json = nlohmann::json::parse(
      run("ffprobe -v error -select_streams v:0 -show_entries packet=pos,size,dts:stream=time_base -of json " +
          quoted(path) + " 2>/dev/null"),
      nullptr, false);
  REQUIRE_FALSE(json.is_discarded());
  const auto& packets = json.at("packets");
  const std::string tb = json.at("streams").at(0).at("time_base").get<std::string>();
  const Opened o(path);
  int video = -1;
  uint32_t timescale = 0;
  for (int k = 0; k < ovt_container_track_count(o.c) && video < 0; ++k) {
    Owned ti;
    REQUIRE(ovt_container_track_info(o.c, k, &ti.b) == OVT_OK);
    const auto* info = flatbuffers::GetRoot<ovt::fb::TrackInfo>(ti.b.data);
    if (info->handler() == ovt::fb::HandlerType::Video) {
      video = k;
      timescale = info->timescale();
    }
  }
  REQUIRE(video >= 0);
  REQUIRE(tb == "1/" + std::to_string(timescale));  // so ffprobe DTS are in track ticks
  const auto table = table_of(o.c, video);
  Owned tl;
  REQUIRE(ovt_container_fragments(o.c, video, nullptr, &tl.b) == OVT_OK);
  const auto* timeline = flatbuffers::GetRoot<ovt::fb::FragmentTimeline>(tl.b.data);
  REQUIRE(timeline->fragments()->size() > 1);
  for (const auto* f : *timeline->fragments()) {
    CHECK(f->dts_gap() == 0);
  }
  const auto has_tfdt = [&](int32_t f) {
    if (f < 0) {
      return true;  // a moov sample
    }
    const auto* trafs = timeline->fragments()->Get(static_cast<uint32_t>(f))->trafs();
    return !trafs->empty() && trafs->Get(0)->has_tfdt();
  };
  const auto n = std::min<size_t>({1000, packets.size(), table->count});
  REQUIRE(n > 0);
  REQUIRE(has_tfdt(table->fragment_index[0]));
  const int64_t ff0 = num(packets.at(0).at("dts"));
  size_t against_ffprobe = 0;
  for (size_t i = 0; i < n; ++i) {
    const auto& p = packets.at(i);
    INFO(path.filename().string() << " packet " << i);
    CHECK(static_cast<int64_t>(table->offset[i]) == num(p.at("pos")));
    CHECK(int64_t{table->size[i]} == num(p.at("size")));
    if (has_tfdt(table->fragment_index[i])) {
      CHECK(table->dts[i] - table->dts[0] == num(p.at("dts")) - ff0);
      ++against_ffprobe;
    } else {
      CHECK(table->dts[i] == table->dts[i - 1] + int64_t{table->duration[i - 1]});
    }
  }
  return {n, against_ffprobe};
}

// The init segment followed by the media segments of a DASH set (seg_init.mp4 / init.mp4, then *.m4s in segment
// number order) as one file in `dir`.
std::filesystem::path concat_segments(const std::filesystem::path& set, const std::filesystem::path& dir) {
  std::optional<std::filesystem::path> init;
  std::vector<std::pair<uint64_t, std::filesystem::path>> segs;
  for (const auto& e : std::filesystem::recursive_directory_iterator(set)) {
    const std::string name = e.path().filename().string();
    if (name.find("init") != std::string::npos && e.path().extension() == ".mp4") {
      init = e.path();
    } else if (e.path().extension() == ".m4s") {
      const auto digits = name.find_first_of("0123456789");
      segs.emplace_back(digits == std::string::npos ? 0 : std::stoull(name.substr(digits)), e.path());
    }
  }
  if (!init || segs.empty()) {
    return {};
  }
  std::ranges::sort(segs);
  const std::filesystem::path out = dir / "concat.mp4";
  std::ofstream os(out, std::ios::binary);
  os << std::ifstream(*init, std::ios::binary).rdbuf();
  for (const auto& [num, p] : segs) {
    os << std::ifstream(p, std::ios::binary).rdbuf();
  }
  return out;
}

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("ovt_frag_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("fragments: offsets, sizes and DTS equal ffprobe on generated fragmented files",
          "[container][fragments][ffprobe]") {
  // Acceptance test 1 on files made here: ffmpeg's own fragmenter and, when MP4Box is present, the MP4Box DASH
  // segmenter of corpus recipe 20 (b) (init + segments with sidx, styp, tfdt; the syn_mp4_frag recipe).
  if (!on_path("ffmpeg") || !on_path("ffprobe")) {
    SKIP("ffmpeg or ffprobe is not available");
  }
  const TempDir tmp;
  const std::filesystem::path plain = tmp.path / "plain.mp4";
  const std::filesystem::path frag = tmp.path / "frag.mp4";
  const std::string src =
      "ffmpeg -v error -y -f lavfi -i testsrc2=size=160x120:rate=25 -frames:v 150 -c:v mpeg4 -bf 2 "
      "-g 25 ";
  run(src + quoted(plain) + " 2>&1");
  run(src + "-movflags +frag_keyframe+empty_moov+default_base_moof " + quoted(frag) + " 2>&1");
  REQUIRE(std::filesystem::exists(frag));
  CHECK(compare_with_ffprobe(frag) == std::pair<size_t, size_t>{150, 150});  // ffmpeg writes tfdt in every moof
  if (on_path("MP4Box")) {
    run("cd " + quoted(tmp.path) +
        " && MP4Box -quiet -for-test -dash 1000 -frag 500 -rap -segment-name seg_ -out syn_mp4_frag.mpd plain.mp4 "
        "2>&1");
    const std::filesystem::path dash = concat_segments(tmp.path, tmp.path);
    REQUIRE_FALSE(dash.empty());
    const auto [n, against_ffprobe] = compare_with_ffprobe(dash);
    CHECK(n == 150);
    CHECK(against_ffprobe > 0);  // MP4Box -frag 500 -dash 1000 writes tfdt only in the first moof of a segment
  }
}

TEST_CASE("fragments: offsets, sizes and DTS equal ffprobe on the fragmented corpus sets",
          "[container][fragments][ffprobe][slow][needs-corpus]") {
  // Acceptance test 1 on syn_mp4_frag (recipe 30 = 20 (b)) and cta_cmaf_video_cmfv (CTA WAVE).
  if (!on_path("ffprobe")) {
    SKIP("ffprobe is not available");
  }
  for (const std::string_view id : {"syn_mp4_frag", "cta_cmaf_video_cmfv"}) {
    DYNAMIC_SECTION(id) {
      const auto primary = ovt::test::corpus_or_skip(id);
      const TempDir tmp;
      const std::filesystem::path file =
          primary.extension() == ".mpd" ? concat_segments(primary.parent_path(), tmp.path) : primary;
      REQUIRE_FALSE(file.empty());
      CHECK(compare_with_ffprobe(file).second > 0);
    }
  }
}

TEST_CASE("fragments: traf sbgp memberships join the SampleTable groups", "[container][fragments][sample_table]") {
  // Fragment 0: sbgp 'seig' → samples 0-1 use the fragment-local entry 1 (0x10001), sample 2 none; one local sgpd.
  // Fragment 1: no sbgp → 0 for its samples.
  TrafSpec t = at(0);
  const Bytes sbgp = box("sbgp", full(0, 0,
                                      cat({Bytes{'s', 'e', 'i', 'g'}, be(2, 4), be(2, 4), be(0x10001, 4), be(5, 4),
                                           be(0, 4)})));  // the second run claims too many samples
  const Bytes sgpd = box("sgpd", full(1, 0, cat({Bytes{'s', 'e', 'i', 'g'}, be(20, 4), be(1, 4), Bytes(20, 0)})));
  t.extra_boxes = cat({sbgp, sgpd});
  const Bytes head = cat({ftyp(), moov({kTrack})});
  const Bytes f0 = fragment(1, {t}, 3 * kSize);
  const Opened o(cat({head, f0, fragment(2, {at(120)}, 3 * kSize)}));
  const auto table = table_of(o.c, 0);
  REQUIRE(table->count == 6);
  REQUIRE(table->groups.size() == 1);
  const auto& g = *table->groups[0];
  CHECK(g.grouping_type == "seig");
  CHECK(g.grouping_type_parameter == 0);
  CHECK(g.description_index == std::vector<uint32_t>{0x10001, 0x10001, 0, 0, 0, 0});
  // The sgpd follows the trun and the sbgp inside the traf, at the end of the moof.
  const uint64_t moof_end = head.size() + f0.size() - (3 * kSize + 8);
  CHECK(g.sgpd_offset == moof_end - sgpd.size());
}
