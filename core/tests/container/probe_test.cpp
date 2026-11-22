// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the core functions behind `ovt probe` (chapter 08 WI-1.235): ovt_container_expand_all, the hex helper of
// the byte service (`--bytes OFFSET:LEN`) and the diagnostics registry the dump reads.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/hex_dump.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/jobs.h"
#include "ovt_container_generated.h"

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes full_box_payload(uint32_t value) {
  Bytes b(4, 0);
  put32(b, value);
  return b;
}

// ftyp, a moov with mvex, then `fragments` of moof(mfhd, traf(tfhd, tfdt, trun)) + mdat. Above kEagerMoofLimit (64)
// fragments every moof starts unexpanded (§8.2.4 rule 3).
Bytes fragmented_file(unsigned fragments) {
  Bytes f = cat({box("ftyp", cat({Bytes{'i', 's', 'o', '6'}, full_box_payload(0)})),
                 box("moov", cat({box("mvhd", Bytes(100, 0)), box("mvex", box("trex", Bytes(24, 0)))}))});
  for (unsigned i = 1; i <= fragments; ++i) {
    const Bytes frag =
        cat({box("moof", cat({box("mfhd", full_box_payload(i)),
                              box("traf", cat({box("tfhd", full_box_payload(1)), box("tfdt", full_box_payload(0)),
                                               box("trun", full_box_payload(0))}))})),
             box("mdat", Bytes(16, 0xAA))});
    f.insert(f.end(), frag.begin(), frag.end());
  }
  return f;
}

struct Opened {
  ovt_File handle;
  ovt_Container* container = nullptr;
  explicit Opened(Bytes data) : handle{std::make_unique<ovt::io::MemoryFile>(std::move(data))} {
    REQUIRE(ovt_container_open(&handle, &container) == OVT_OK);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  ~Opened() { ovt_container_close(container); }
};

// Counts nodes carrying CNT-0007, split by `mdat` and everything else.
struct Unexpanded {
  size_t outside_mdat = 0;
  size_t in_mdat = 0;
  size_t moof_with_traf = 0;
};

void walk(const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::BoxNode>>* nodes, Unexpanded& u) {
  if (nodes == nullptr) {
    return;
  }
  for (const auto* n : *nodes) {
    const bool marked = n->diagnostics() != nullptr && std::ranges::any_of(*n->diagnostics(), [](const auto* d) {
                          return d->code()->string_view() == "CNT-0007";
                        });
    if (marked) {
      ++(n->fourcc()->string_view() == "mdat" ? u.in_mdat : u.outside_mdat);
    }
    if (n->fourcc()->string_view() == "traf") {
      ++u.moof_with_traf;
    }
    walk(n->children(), u);  // NOLINT(misc-no-recursion): bounded by the test tree
  }
}

Unexpanded survey(ovt_Container* c) {
  const ovt_Buffer b = ovt_container_tree(c);
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::ContainerTree>(nullptr));
  Unexpanded u;
  walk(flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data)->root(), u);
  return u;
}

}  // namespace

TEST_CASE("probe: expand_all leaves no unexpanded box outside mdat", "[container][probe][fast]") {
  Opened o(fragmented_file(200));
  const Unexpanded before = survey(o.container);
  CHECK(before.outside_mdat == 200);  // every moof is deferred
  CHECK(before.moof_with_traf == 0);

  CHECK(ovt_container_expand_all(o.container, nullptr) == OVT_OK);
  const Unexpanded after = survey(o.container);
  CHECK(after.outside_mdat == 0);
  CHECK(after.in_mdat == 0);
  CHECK(after.moof_with_traf == 200);
  // Idempotent: a second call changes nothing and succeeds.
  CHECK(ovt_container_expand_all(o.container, nullptr) == OVT_OK);
  CHECK(survey(o.container).moof_with_traf == 200);
}

TEST_CASE("probe: expand_all is deterministic", "[container][probe][fast]") {
  const auto expanded_tree = [] {
    Opened o(fragmented_file(100));
    REQUIRE(ovt_container_expand_all(o.container, nullptr) == OVT_OK);
    const ovt_Buffer b = ovt_container_tree(o.container);
    return Bytes(b.data, b.data + b.size);
  };
  CHECK(expanded_tree() == expanded_tree());
}

TEST_CASE("probe: expand_all under a job reports progress and honours cancellation", "[container][probe][fast]") {
  ovt_Job* job = nullptr;
  REQUIRE(ovt_job_create(&job) == OVT_OK);
  {
    Opened o(fragmented_file(100));
    CHECK(ovt_container_expand_all(o.container, job) == OVT_OK);
    CHECK(survey(o.container).outside_mdat == 0);
  }
  {
    Opened o(fragmented_file(100));
    ovt_job_cancel(job);
    CHECK(ovt_container_expand_all(o.container, job) == OVT_E_CANCELLED);
    CHECK(survey(o.container).outside_mdat == 100);  // nothing was expanded
    ovt_job_reset(job);
    CHECK(ovt_container_expand_all(o.container, job) == OVT_OK);  // the handle is still usable afterwards
    CHECK(survey(o.container).outside_mdat == 0);
  }
  ovt_job_free(job);
}

TEST_CASE("probe: expand_all on a container without lazy boxes succeeds", "[container][probe][fast]") {
  const Bytes y4m = [] {
    const std::string head = "YUV4MPEG2 W2 H2 F25:1 Ip A1:1 C420jpeg\n";
    Bytes b(head.begin(), head.end());
    for (int i = 0; i < 3; ++i) {
      const std::string fr = "FRAME\n";
      b.insert(b.end(), fr.begin(), fr.end());
      b.insert(b.end(), 6, 0x80);
    }
    return b;
  }();
  Opened o(y4m);
  CHECK(ovt_container_expand_all(o.container, nullptr) == OVT_OK);
  CHECK(survey(o.container).outside_mdat == 0);
}

TEST_CASE("probe: hex dump rows have an offset, two groups of eight and an ASCII column", "[container][probe][fast]") {
  const Bytes bytes = {'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0, 'i', 's', 'o', 'm', 'i', 's', 'o', '2'};
  using ovt::container::format_hex_dump;
  CHECK(format_hex_dump(0, std::span<const uint8_t>(bytes.data(), 16)) ==
        "00000000  66 74 79 70 69 73 6f 6d  00 00 02 00 69 73 6f 6d  |ftypisom....isom|\n");
  // A short last row is padded so that the ASCII column lines up; the first row starts at the given offset.
  CHECK(format_hex_dump(0x10, std::span<const uint8_t>(bytes.data() + 16, 4)) ==
        "00000010  69 73 6f 32                                       |iso2|\n");
  CHECK(format_hex_dump(0, {}).empty());
  // 17 bytes: two rows, the second with one byte.
  const std::string two = format_hex_dump(0, std::span<const uint8_t>(bytes.data(), 17));
  CHECK(std::ranges::count(two, '\n') == 2);
  // Non-printable and high bytes show as '.'.
  const Bytes odd = {0x00, 0x1f, 0x20, 0x7e, 0x7f, 0xff};
  CHECK(format_hex_dump(0, odd).ends_with("|.. ~..|\n"));
  // A large offset widens the offset column of every row.
  const std::string wide = format_hex_dump(0x1'0000'0000ULL, std::span<const uint8_t>(bytes.data(), 1));
  CHECK(wide.starts_with("100000000  66 "));
}

TEST_CASE("probe: hex_dump_bytes reads through the byte service and clips at EOF", "[container][probe][fast]") {
  Opened o(fragmented_file(1));
  std::string out;
  REQUIRE(ovt::container::hex_dump_bytes(o.container, 0, 16, &out) == OVT_OK);
  CHECK(out.starts_with("00000000  "));
  CHECK(out.find('|') != std::string::npos);
  CHECK(out.find("ftyp") != std::string::npos);  // ASCII column of the first box
  const uint64_t size = ovt_container_size(o.container);
  REQUIRE(ovt::container::hex_dump_bytes(o.container, size - 5, 16, &out) == OVT_OK);
  CHECK(std::ranges::count(out, '\n') == 1);
  REQUIRE(ovt::container::hex_dump_bytes(o.container, size + 100, 16, &out) == OVT_OK);
  CHECK(out.empty());
  CHECK(ovt::container::hex_dump_bytes(o.container, 0, (1U << 20U) + 1, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt::container::hex_dump_bytes(nullptr, 0, 16, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt::container::hex_dump_bytes(o.container, 0, 16, nullptr) == OVT_E_INVALID_ARG);
}

TEST_CASE("probe: every registry code appears exactly once with a severity", "[container][probe][fast]") {
  std::set<std::string_view> seen;
  for (const auto& d : ovt::container::all_diagnostics()) {
    CHECK(d.code.starts_with("CNT-"));
    CHECK(seen.insert(d.code).second);
    CHECK_FALSE(d.message.empty());
  }
  CHECK_FALSE(seen.empty());
  // The "≥ 150 codes" bound of WI-1.235 AT2 is not asserted here: the registry holds the codes of the work items
  // merged so far (see the PR description); the C function that returns it is a spec question.
}
