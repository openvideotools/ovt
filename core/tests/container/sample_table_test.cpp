// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the SampleTable expansion (chapter 08 §8.4.1, WI-1.218) over synthetic MP4 files: the formulas of steps 1-9,
// every diagnostic the expansion raises, the sample-count and overflow bounds, the C API and the MP4Box -dts goldens.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "container/isobmff/sample_table.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
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
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
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
  put_str(b, type);
  put_all(b, payload);
  return b;
}
Bytes full_box(std::string_view type, uint32_t version_flags, const Bytes& payload) {
  Bytes p;
  put(p, version_flags, 4);
  put_all(p, payload);
  return box(type, p);
}

// A table box: entry_count then the entries, each a list of (value, width) pairs.
Bytes u32s(std::initializer_list<uint32_t> values) {
  Bytes p;
  for (const uint32_t v : values) {
    put(p, v, 4);
  }
  return p;
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

Bytes tkhd(uint32_t id) {
  Bytes p;
  put(p, 0, 8);
  put(p, id, 4);
  p.insert(p.end(), 4 + 4 + 8 + 8 + 36 + 8, 0);
  return full_box("tkhd", 3, p);
}
Bytes mdhd(uint32_t timescale) {
  Bytes p;
  put(p, 0, 8);
  put(p, timescale, 4);
  put(p, 0, 4);
  put(p, 0x55C4, 2);
  put(p, 0, 2);
  return full_box("mdhd", 0, p);
}
Bytes stsd(uint32_t entries) {
  Bytes p;
  put(p, entries, 4);
  for (uint32_t i = 0; i < entries; ++i) {
    put_all(p, box("avc1", Bytes(78, 0)));
  }
  return full_box("stsd", 0, p);
}
Bytes stsz(uint32_t fixed, const std::vector<uint32_t>& sizes, uint32_t declared = UINT32_MAX) {
  Bytes p;
  put(p, fixed, 4);
  put(p, declared == UINT32_MAX ? sizes.size() : declared, 4);
  for (const uint32_t s : sizes) {
    put(p, s, 4);
  }
  return full_box("stsz", 0, p);
}
Bytes co64(const std::vector<uint64_t>& offsets) {
  Bytes p;
  put(p, offsets.size(), 4);
  for (const uint64_t o : offsets) {
    put(p, o, 8);
  }
  return full_box("co64", 0, p);
}

// One track whose stbl holds `boxes` after stsd (1 entry).
Bytes trak(uint32_t id, const std::vector<Bytes>& boxes, uint32_t stsd_entries = 1) {
  Bytes stbl = stsd(stsd_entries);
  for (const Bytes& b : boxes) {
    put_all(stbl, b);
  }
  return box("trak", cat({tkhd(id), box("mdia", cat({mdhd(1000), box("minf", box("stbl", stbl))}))}));
}

// ftyp, moov, mdat of `mdat_bytes`; `make(mdat_payload_offset)` builds the moov children once the position of the mdat
// payload is known (the moov size must not depend on it).
Bytes mp4(uint32_t mdat_bytes, const auto& make) {
  Bytes ftyp;
  put_str(ftyp, "isom");
  put(ftyp, 0, 4);
  const auto build = [&](uint64_t mdat_payload) {
    Bytes children;
    for (const Bytes& c : make(mdat_payload)) {
      put_all(children, c);
    }
    return cat({box("ftyp", ftyp), box("moov", children)});
  };
  const uint64_t head = build(0).size();
  return cat({build(head + 8), box("mdat", Bytes(mdat_bytes, 0))});
}

struct Expanded {
  Bytes data;
  std::unique_ptr<ovt::fb::SampleTableT> t;
  [[nodiscard]] const ovt::fb::DiagnosticT* diag(std::string_view code) const {
    const auto it = std::ranges::find_if(t->diagnostics, [&](const auto& d) { return d->code == code; });
    return it == t->diagnostics.end() ? nullptr : it->get();
  }
  [[nodiscard]] bool has(std::string_view code) const { return diag(code) != nullptr; }
};

Expanded expand(Bytes data, size_t trak_index = 0) {
  Expanded e;
  e.data = std::move(data);
  const ovt::io::MemoryFile file(e.data);
  const iso::WalkResult w = iso::walk(file);
  const ovt::fb::BoxNodeT* trak = iso::find_trak(w.top_level, trak_index);
  REQUIRE(trak != nullptr);
  e.t = iso::expand_sample_table(file, *trak, ovt::jobs::Cancel::never());
  return e;
}

// A file with one track: `boxes` after stsd, an mdat of `mdat_bytes`; the stco box is built by `stco(mdat_payload)`.
Expanded expand_boxes(const std::vector<Bytes>& boxes, uint32_t mdat_bytes = 1000) {
  return expand(mp4(mdat_bytes, [&](uint64_t) { return std::vector<Bytes>{trak(1, boxes)}; }));
}

}  // namespace

TEST_CASE("sample table: formulas of §8.4.1 on a six-sample track", "[container][sample_table][fast]") {
  // Chunks 1-2 hold two samples, chunks 3-4 one: 2 + 2 + 1 + 1 = 6 samples.
  const Bytes data = mp4(600, [](uint64_t m) {
    return std::vector<Bytes>{
        trak(1, {table("stts", 0, {{4, 100}, {2, 50}}), table("ctts", 0x01000000, {{3, 200}, {3, 0xFFFFFF9C}}),  // -100
                 table("stsc", 0, {{1, 2, 1}, {3, 1, 1}}), stsz(0, {10, 20, 30, 40, 50, 60}),
                 co64({m, m + 100, m + 200, m + 300}), table("stss", 0, {{1}, {4}})})};
  });
  const Expanded e = expand(data);
  const auto& t = *e.t;
  CHECK(t.track_id == 1);
  CHECK(t.timescale == 1000);
  CHECK(t.count == 6);
  CHECK(t.diagnostics.empty());
  CHECK(t.dts == std::vector<int64_t>{0, 100, 200, 300, 400, 450});
  CHECK(t.duration == std::vector<uint32_t>{100, 100, 100, 100, 50, 50});
  CHECK(t.cts_offset == std::vector<int32_t>{200, 200, 200, -100, -100, -100});
  CHECK(t.size == std::vector<uint32_t>{10, 20, 30, 40, 50, 60});
  CHECK(t.chunk_index == std::vector<uint32_t>{1, 1, 2, 2, 3, 4});
  CHECK(t.sample_description_index == std::vector<uint16_t>(6, 1));
  CHECK(t.fragment_index == std::vector<int32_t>(6, -1));
  CHECK(t.is_sync == std::vector<bool>{true, false, false, true, false, false});
  CHECK(t.is_partial_sync.empty());
  CHECK(t.sdtp.empty());
  CHECK(t.composition_to_dts_shift == 100);  // derived max(0, -min(cts_offset))
  const uint64_t m = t.offset[0];
  CHECK(t.offset == std::vector<uint64_t>{m, m + 10, m + 100, m + 130, m + 200, m + 300});
}

TEST_CASE("sample table: no stss means every sample is a sync sample", "[container][sample_table][fast]") {
  const Expanded e =
      expand_boxes({table("stts", 0, {{2, 10}}), stsz(5, {}, 2), table("stsc", 0, {{1, 2, 1}}), co64({40})});
  CHECK(e.t->count == 2);
  CHECK(e.t->size == std::vector<uint32_t>{5, 5});
  CHECK(e.t->is_sync == std::vector<bool>{true, true});
  CHECK(e.t->cts_offset == std::vector<int32_t>{0, 0});
  CHECK(e.t->composition_to_dts_shift == 0);
}

TEST_CASE("sample table: stz2 field sizes 4, 8 and 16", "[container][sample_table][fast]") {
  const auto stz2 = [](uint8_t field, const Bytes& entries, uint32_t count) {
    Bytes p;
    put(p, 0, 3);
    p.push_back(field);
    put(p, count, 4);
    put_all(p, entries);
    return full_box("stz2", 0, p);
  };
  const auto sizes = [&](const Bytes& z) {
    return expand_boxes({table("stts", 0, {{3, 1}}), z, table("stsc", 0, {{1, 3, 1}}), co64({40})}).t->size;
  };
  CHECK(sizes(stz2(4, Bytes{0x12, 0x30}, 3)) == std::vector<uint32_t>{1, 2, 3});  // high nibble first
  CHECK(sizes(stz2(8, Bytes{7, 8, 9}, 3)) == std::vector<uint32_t>{7, 8, 9});
  CHECK(sizes(stz2(16, Bytes{1, 0, 0, 2, 1, 1}, 3)) == std::vector<uint32_t>{256, 2, 257});
  const Expanded bad = expand_boxes({table("stts", 0, {{3, 1}}), stz2(5, Bytes(8, 0), 3)});
  CHECK(bad.has("CNT-2006"));
  CHECK(bad.t->count == 0);
}

TEST_CASE("sample table: stts versus stsz (CNT-2005) and zero deltas (CNT-2001)", "[container][sample_table][fast]") {
  const Expanded few =
      expand_boxes({table("stts", 0, {{2, 10}}), stsz(1, {}, 4), table("stsc", 0, {{1, 4, 1}}), co64({40})});
  CHECK(few.has("CNT-2005"));
  CHECK(few.t->count == 2);
  CHECK(few.t->dts == std::vector<int64_t>{0, 10});
  const Expanded many =
      expand_boxes({table("stts", 0, {{5, 10}}), stsz(1, {}, 3), table("stsc", 0, {{1, 3, 1}}), co64({40})});
  CHECK(many.has("CNT-2005"));
  CHECK(many.t->count == 3);

  const Expanded zero = expand_boxes(
      {table("stts", 0, {{2, 0}, {1, 5}, {1, 0}}), stsz(1, {}, 4), table("stsc", 0, {{1, 4, 1}}), co64({40})});
  const auto* d = zero.diag("CNT-2001");
  REQUIRE(d != nullptr);
  CHECK(d->message.find("2 non-last") != std::string::npos);  // samples 0 and 1; the last sample may be 0
  CHECK_FALSE(zero.has("CNT-2005"));
}

TEST_CASE("sample table: ctts versions, CNT-2002 and CNT-2011", "[container][sample_table][fast]") {
  const std::vector<Bytes> rest = {table("stts", 0, {{3, 10}}), stsz(1, {}, 3), table("stsc", 0, {{1, 3, 1}}),
                                   co64({40})};
  const auto with = [&](const Bytes& ctts) {
    std::vector<Bytes> b = rest;
    b.push_back(ctts);
    return expand_boxes(b);
  };
  const Expanded v0 = with(table("ctts", 0, {{1, 0x80000000U}, {2, 4}}));
  CHECK(v0.has("CNT-2002"));
  CHECK(v0.t->cts_offset == std::vector<int32_t>{INT32_MIN, 4, 4});
  const Expanded v1 = with(table("ctts", 0x01000000, {{1, 0x80000000U}, {2, 4}}));
  CHECK_FALSE(v1.has("CNT-2002"));
  const Expanded shortrun = with(table("ctts", 0, {{2, 7}}));
  CHECK(shortrun.has("CNT-2011"));
  CHECK(shortrun.t->cts_offset == std::vector<int32_t>{7, 7, 0});
  CHECK(shortrun.t->count == 3);  // the table keeps every sample
}

TEST_CASE("sample table: cslg is informative (CNT-2012)", "[container][sample_table][fast]") {
  const std::vector<Bytes> base = {table("stts", 0, {{2, 10}}), stsz(1, {}, 2), table("stsc", 0, {{1, 2, 1}}),
                                   co64({40}), table("ctts", 0x01000000, {{1, 0}, {1, 0xFFFFFFFB}})};  // offsets 0, -5
  const auto with = [&](uint32_t shift, uint32_t least, uint32_t greatest) {
    std::vector<Bytes> b = base;
    b.push_back(full_box("cslg", 0, u32s({shift, least, greatest, 0, 0})));
    return expand_boxes(b);
  };
  const Expanded ok = with(5, 0xFFFFFFFB, 0);
  CHECK_FALSE(ok.has("CNT-2012"));
  CHECK(ok.t->composition_to_dts_shift == 5);
  CHECK(ok.t->cts_offset == std::vector<int32_t>{0, -5});  // CTS never includes cslg
  CHECK(with(3, 0xFFFFFFFB, 0).has("CNT-2012"));           // shift smaller than the derived 5
  CHECK(with(5, 0xFFFFFFFC, 0).has("CNT-2012"));           // least delta differs from min(cts_offset)
}

TEST_CASE("sample table: zero-size samples are an Info (CNT-2013)", "[container][sample_table][fast]") {
  const Expanded e =
      expand_boxes({table("stts", 0, {{3, 1}}), stsz(0, {5, 0, 0}), table("stsc", 0, {{1, 3, 1}}), co64({40})});
  const auto* d = e.diag("CNT-2013");
  REQUIRE(d != nullptr);
  CHECK(d->message.find("2 zero-size") != std::string::npos);
}

TEST_CASE("sample table: chunk map findings (CNT-2003, CNT-2014, CNT-2015)", "[container][sample_table][fast]") {
  const auto run = [](const Bytes& stsc, const std::vector<uint64_t>& offsets, uint32_t samples) {
    return expand_boxes({table("stts", 0, {{samples, 1}}), stsz(4, {}, samples), stsc, co64(offsets)});
  };
  const Expanded more = run(table("stsc", 0, {{1, 2, 1}}), {40, 60, 80}, 3);
  CHECK(more.has("CNT-2014"));
  CHECK(more.t->count == 3);
  CHECK(more.t->chunk_index == std::vector<uint32_t>{1, 1, 2});
  const Expanded none = run(table("stsc", 0, {{1, 1, 1}}), {40}, 3);
  const auto* d = none.diag("CNT-2015");
  REQUIRE(d != nullptr);
  CHECK(d->message.find("2 samples have no chunk") != std::string::npos);
  CHECK(none.t->count == 3);
  CHECK(none.t->offset[1] == 0);
  CHECK(none.t->chunk_index[2] == 0);
  const Expanded late = run(table("stsc", 0, {{2, 1, 1}}), {40, 50}, 2);  // first_chunk of entry 0 is not 1
  CHECK(late.has("CNT-2003"));
  CHECK(late.has("CNT-2015"));
  const Expanded rising = run(table("stsc", 0, {{1, 1, 1}, {1, 1, 1}}), {40, 50}, 2);
  CHECK(rising.has("CNT-2003"));
  const Expanded beyond = run(table("stsc", 0, {{1, 1, 1}, {9, 1, 1}}), {40, 50}, 2);
  CHECK(beyond.has("CNT-2003"));
}

TEST_CASE("sample table: stco beyond the end of the file (AT3, CNT-2007)", "[container][sample_table][fast]") {
  // Samples 0 and 1 sit in chunk 1 at a valid place; chunk 2 starts far beyond the end of the file.
  const Bytes data = mp4(100, [](uint64_t m) {
    return std::vector<Bytes>{trak(1, {
                                          table("stts", 0, {{4, 1}}),
                                          stsz(10, {}, 4),
                                          table("stsc", 0, {{1, 2, 1}}),
                                          co64({m, 0x7FFFFFF0ULL}),
                                      })};
  });
  const Expanded e = expand(data);
  const auto* d = e.diag("CNT-2007");
  REQUIRE(d != nullptr);
  CHECK(d->message.find("sample 2 ") != std::string::npos);  // the first offending sample
  CHECK(std::ranges::count_if(e.t->diagnostics, [](const auto& x) { return x->code == "CNT-2007"; }) ==
        2);  // samples 2 and 3
  CHECK(e.t->count == 4);
}

TEST_CASE("sample table: CNT-2007 lists 16 samples and then a total", "[container][sample_table][fast]") {
  const Expanded e = expand_boxes(
      {table("stts", 0, {{40, 1}}), stsz(10, {}, 40), table("stsc", 0, {{1, 40, 1}}), co64({0x7FFFFFF0ULL})});
  CHECK(std::ranges::count_if(e.t->diagnostics, [](const auto& x) { return x->code == "CNT-2007"; }) == 17);
  CHECK(e.t->diagnostics.back()->message.find("40 samples in total") != std::string::npos);
}

// SPEC-ASSUMPTION(#785): AT4 as worded (Σ stts deltas > 2^63) is unreachable under the 50 M sample bound; the overflow
// is exercised through the chunk offset sum.
TEST_CASE("sample table: 64-bit overflow stops the expansion (AT4, CNT-2030)", "[container][sample_table][fast]") {
  // The chunk offset is 2^64 - 150: sample 0 ends at 2^64 - 50, sample 1 would end past 2^64.
  const Expanded e = expand_boxes(
      {table("stts", 0, {{4, 1}}), stsz(100, {}, 4), table("stsc", 0, {{1, 4, 1}}), co64({UINT64_MAX - 149})});
  const auto* d = e.diag("CNT-2030");
  REQUIRE(d != nullptr);
  CHECK(d->message.find("sample 2") != std::string::npos);
  CHECK(e.t->count == 2);
  CHECK(e.t->dts.size() == 2);
  CHECK(e.t->offset == std::vector<uint64_t>{UINT64_MAX - 149, UINT64_MAX - 49});
  CHECK(e.has("CNT-2007"));  // both lie beyond the end of the file
  CHECK_FALSE(e.has("CNT-2015"));
}

TEST_CASE("sample table: a claimed count above the bound is truncated (CNT-2031)", "[container][sample_table][fast]") {
  // stsz claims 4 billion samples of 1 byte; the table keeps what stts and the bound allow.
  const Expanded e =
      expand_boxes({table("stts", 0, {{3, 1}}), stsz(1, {}, 4000000000U), table("stsc", 0, {{1, 3, 1}}), co64({40})});
  CHECK(e.has("CNT-2031"));
  CHECK(e.has("CNT-2005"));
  CHECK(e.t->count == 3);
}

// SPEC-ASSUMPTION(#788): a constant-size stsz claims no more samples than the file can hold.
TEST_CASE("sample table: a constant sample size is bounded by the file size", "[container][sample_table][fast]") {
  const Expanded e = expand_boxes({table("stts", 0, {{50'000'000, 1}}), stsz(1000, {}, 50'000'000),
                                   table("stsc", 0, {{1, 50'000'000, 1}}), co64({40})});
  const auto* d = e.diag("CNT-2031");
  REQUIRE(d != nullptr);
  CHECK(d->message.find("holds at most") != std::string::npos);
  CHECK(e.t->count < 5000);  // the file is a few KiB
}

TEST_CASE("sample table: stss, stps, sdtp and stdp", "[container][sample_table][fast]") {
  const std::vector<Bytes> base = {table("stts", 0, {{4, 1}}), stsz(1, {}, 4), table("stsc", 0, {{1, 4, 1}}),
                                   co64({40})};
  const auto with = [&](std::vector<Bytes> more) {
    std::vector<Bytes> b = base;
    b.insert(b.end(), more.begin(), more.end());
    return expand_boxes(b);
  };
  const Expanded bad = with({table("stss", 0, {{2}, {0}, {9}, {2}})});  // 0, past the count and a repeat are ignored
  CHECK(bad.has("CNT-2009"));
  CHECK(bad.t->is_sync == std::vector<bool>{false, true, false, false});

  const Expanded sdtp_short = with({full_box("sdtp", 0, Bytes{0x20, 0x20})});
  CHECK(sdtp_short.has("CNT-2010"));
  CHECK(sdtp_short.t->sdtp == std::vector<uint8_t>{0x20, 0x20, 0, 0});

  // sample_depends_on == 2 is an I picture; sample 2 (index 1) is not in stss.
  const Expanded open_gop = with({table("stss", 0, {{1}}), full_box("sdtp", 0, Bytes{0x20, 0x20, 0x10, 0x10})});
  CHECK(open_gop.has("CNT-2018"));
  CHECK_FALSE(open_gop.has("CNT-2010"));

  const Expanded extras =
      with({table("stps", 0, {{3}}), full_box("stdp", 0, Bytes{0, 1, 0, 2, 0, 3, 0, 4}), box("subs")});
  CHECK(extras.t->is_partial_sync == std::vector<bool>{false, false, true, false});
  CHECK(extras.t->degradation_priority == std::vector<uint16_t>{1, 2, 3, 4});
  CHECK(extras.t->has_subsamples);
}

TEST_CASE("sample table: sample groups (CNT-2021)", "[container][sample_table][fast]") {
  const auto sbgp = [](std::string_view type, uint32_t version, const std::vector<std::vector<uint32_t>>& rows) {
    Bytes p;
    put_str(p, type);
    if (version == 1) {
      put(p, 7, 4);  // grouping_type_parameter
    }
    put(p, rows.size(), 4);
    for (const auto& r : rows) {
      put(p, r[0], 4);
      put(p, r[1], 4);
    }
    return full_box("sbgp", version << 24U, p);
  };
  const auto sgpd = [](std::string_view type, uint32_t entries) {
    Bytes p;
    put_str(p, type);
    put(p, entries, 4);
    for (uint32_t i = 0; i < entries; ++i) {
      put(p, 0, 2);  // roll_distance
    }
    return full_box("sgpd", 0, p);
  };
  const std::vector<Bytes> base = {table("stts", 0, {{4, 1}}), stsz(1, {}, 4), table("stsc", 0, {{1, 4, 1}}),
                                   co64({40})};
  const auto with = [&](std::vector<Bytes> more) {
    std::vector<Bytes> b = base;
    b.insert(b.end(), more.begin(), more.end());
    return expand_boxes(b);
  };
  const Expanded ok = with({sbgp("roll", 1, {{1, 0}, {2, 2}, {1, 1}}), sgpd("roll", 2)});
  REQUIRE(ok.t->groups.size() == 1);
  CHECK(ok.t->groups[0]->grouping_type == "roll");
  CHECK(ok.t->groups[0]->grouping_type_parameter == 7);
  CHECK(ok.t->groups[0]->description_index == std::vector<uint32_t>{0, 2, 2, 1});
  CHECK(ok.t->groups[0]->sgpd_offset > 0);
  CHECK_FALSE(ok.has("CNT-2021"));
  CHECK(with({sbgp("roll", 0, {{4, 1}})}).has("CNT-2021"));                   // no sgpd
  CHECK(with({sbgp("roll", 0, {{4, 3}}), sgpd("roll", 2)}).has("CNT-2021"));  // index beyond the entries
  CHECK(with({sbgp("roll", 0, {{4, 0x10001}}), sgpd("roll", 2)}).t->groups[0]->description_index[0] ==
        0x10001);  // local
}

TEST_CASE("sample table: stsd.entry_count bounds sample_description_index (CNT-2004)",
          "[container][sample_table][fast]") {
  const auto run = [](uint32_t sdi) {
    return expand(mp4(100, [&](uint64_t m) {
      return std::vector<Bytes>{
          trak(1, {table("stts", 0, {{2, 1}}), stsz(1, {}, 2), table("stsc", 0, {{1, 2, sdi}}), co64({m})})};
    }));
  };
  CHECK_FALSE(run(1).has("CNT-2004"));
  CHECK(run(2).has("CNT-2004"));
  CHECK(run(0).has("CNT-2004"));
}

TEST_CASE("sample table: a trak without stbl or stsz has no samples", "[container][sample_table][fast]") {
  CHECK(expand_boxes({}).t->count == 0);
  const Expanded e = expand_boxes({table("stts", 0, {{2, 1}})});
  CHECK(e.t->count == 0);
  CHECK(e.t->diagnostics.empty());
}

TEST_CASE("sample table: the second trak is addressed by ordinal", "[container][sample_table][fast]") {
  const Bytes data = mp4(100, [](uint64_t m) {
    return std::vector<Bytes>{
        trak(7, {table("stts", 0, {{1, 1}}), stsz(5, {}, 1), table("stsc", 0, {{1, 1, 1}}), co64({m})}),
        trak(7, {table("stts", 0, {{2, 1}}), stsz(6, {}, 2), table("stsc", 0, {{1, 2, 1}}), co64({m})})};
  });
  CHECK(expand(data, 0).t->count == 1);
  CHECK(expand(data, 1).t->count == 2);  // same track_ID, distinct table
}

TEST_CASE("sample table: cancellation", "[container][sample_table][fast]") {
  const Bytes data = mp4(100, [](uint64_t m) {
    return std::vector<Bytes>{
        trak(1, {table("stts", 0, {{2, 1}}), stsz(1, {}, 2), table("stsc", 0, {{1, 2, 1}}), co64({m})})};
  });
  const ovt::io::MemoryFile file(data);
  const iso::WalkResult w = iso::walk(file);
  ovt::jobs::Cancel cancel;
  cancel.request();
  CHECK_THROWS_AS(iso::expand_sample_table(file, *iso::find_trak(w.top_level, 0), cancel), ovt::Cancelled);
}

TEST_CASE("sample table: slice and serialise", "[container][sample_table][fast]") {
  const Expanded e = expand_boxes({table("stts", 0, {{4, 10}}), stsz(5, {}, 4), table("stsc", 0, {{1, 4, 1}}),
                                   co64({40}), table("stss", 0, {{1}, {3}})});
  const auto cut = iso::slice_sample_table(*e.t, 1, 2);
  CHECK(cut->count == 2);
  CHECK(cut->dts == std::vector<int64_t>{10, 20});
  CHECK(cut->is_sync == std::vector<bool>{false, true});
  CHECK(iso::slice_sample_table(*e.t, 0, 0)->count == 4);
  CHECK(iso::slice_sample_table(*e.t, 3, 100)->count == 1);
  const flatbuffers::DetachedBuffer buf = iso::serialise(*e.t);
  flatbuffers::Verifier v(buf.data(), buf.size());
  REQUIRE(v.VerifyBuffer<ovt::fb::SampleTable>(nullptr));
  CHECK(flatbuffers::GetRoot<ovt::fb::SampleTable>(buf.data())->count() == 4);
}

TEST_CASE("sample table: ovt_container_sample_table", "[container][sample_table][c_api][fast]") {
  const Bytes data = mp4(100, [](uint64_t m) {
    return std::vector<Bytes>{
        trak(1, {table("stts", 0, {{4, 10}}), stsz(5, {}, 4), table("stsc", 0, {{1, 4, 1}}), co64({m})})};
  });
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(data)};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  ovt_Buffer b{};
  REQUIRE(ovt_container_sample_table(c, 0, 1, 2, &b) == OVT_OK);
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::SampleTable>(nullptr));
  const auto* t = flatbuffers::GetRoot<ovt::fb::SampleTable>(b.data);
  CHECK(t->track_id() == 1);
  CHECK(t->count() == 2);
  CHECK(t->dts()->Get(0) == 10);
  delete[] b.data;                                                // §12.3.1 rule 3a
  REQUIRE(ovt_container_sample_table(c, 0, 0, 0, &b) == OVT_OK);  // count 0 = to the end
  CHECK(flatbuffers::GetRoot<ovt::fb::SampleTable>(b.data)->count() == 4);
  delete[] b.data;
  CHECK(ovt_container_sample_table(c, 1, 0, 0, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_sample_table(c, -1, 0, 0, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_sample_table(nullptr, 0, 0, 0, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_sample_table(c, 0, 0, 0, nullptr) == OVT_E_INVALID_ARG);
  ovt_container_close(c);
}

TEST_CASE("sample table rows equal the MP4Box -dts goldens", "[container][sample_table][golden][needs-corpus]") {
  const std::filesystem::path dir = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/isobmff";
  size_t compared = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    constexpr std::string_view suffix = ".mp4box-dts.json";
    if (!name.ends_with(suffix)) {
      continue;
    }
    const std::string id = name.substr(0, name.size() - suffix.size());
    const auto path = ovt::test::corpus_path(id);
    if (!path) {
      continue;
    }
    DYNAMIC_SECTION(id) {
      const auto file = ovt::io::RandomAccessFile::open(path->string());
      const iso::WalkResult w = iso::walk(*file);
      std::ifstream gf(entry.path());
      const auto golden = nlohmann::json::parse(gf);
      std::map<uint32_t, std::vector<const nlohmann::json*>> rows;
      std::vector<std::unique_ptr<ovt::fb::SampleTableT>> tables;
      for (size_t i = 0; iso::find_trak(w.top_level, i) != nullptr; ++i) {
        tables.push_back(iso::expand_sample_table(*file, *iso::find_trak(w.top_level, i), ovt::jobs::Cancel::never()));
      }
      for (const auto& r : golden) {
        rows[r.contains("track") ? r.at("track").get<uint32_t>() : tables.at(0)->track_id].push_back(&r);
      }
      for (const auto& t : tables) {
        // A track MP4Box prints no row for (a header-only file) has no golden rows.
        static const std::vector<const nlohmann::json*> kNone;
        const auto it = rows.find(t->track_id);
        const std::vector<const nlohmann::json*>& list = it == rows.end() ? kNone : it->second;
        // Fragment samples follow the moov samples of a track in the golden; the moov table is the prefix.
        // A file without a moof has no fragment samples: the table must match every row, not only a prefix.
        const bool fragmented = std::ranges::any_of(w.top_level, [](const auto& n) { return n->fourcc == "moof"; });
        if (fragmented) {
          REQUIRE(t->count <= list.size());
        } else {
          REQUIRE(t->count == list.size());
        }
        for (uint64_t i = 0; i < t->count; ++i) {
          const nlohmann::json& g = *list[i];
          INFO(id + " track " + std::to_string(t->track_id) + " sample " + std::to_string(i));
          CHECK(t->offset[i] == g.at("offset").get<uint64_t>());
          CHECK(t->size[i] == g.at("size").get<uint32_t>());
          CHECK(t->dts[i] == g.at("dts").get<int64_t>());
          CHECK(t->dts[i] + t->cts_offset[i] == g.at("cts").get<int64_t>());
          CHECK(t->is_sync[i] == (g.at("sync").get<int>() != 0));
          ++compared;
        }
      }
    }
  }
  if (compared == 0) {
    SKIP("no ISOBMFF corpus file with a -dts golden present");
  }
}
