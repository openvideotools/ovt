// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group E part 1 hand decoders (chapter 08 §8.3.5, WI-1.211): trex/tfhd/trun field values, flag-driven
// layouts, the sample_flags bitfield, large-run summaries, CNT-1011 / CNT-4002 / CNT-4003 diagnostics and the field
// comparison against the MP4Box goldens of the container-smoke corpus.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/decoders/fragment_boxes.h"
#include "container/isobmff/field_builder.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"
#include "test_support/golden_compare.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;
using ovt::fb::FieldT;

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
  Bytes t(type.begin(), type.end());
  return cat({be(8 + payload.size(), 4), t, payload});
}
Bytes full(uint8_t version, uint32_t flags, const Bytes& body) {
  return cat({be(version, 1), be(flags, 3), body});
}

iso::FourCC cc_of(std::string_view s) {
  iso::FourCC cc{};
  std::copy_n(s.begin(), 4, cc.begin());
  return cc;
}

iso::BoxContext ctx_in(std::string_view parent) {
  iso::BoxContext c;
  c.ancestors.push_back(cc_of(parent));
  c.trex_for_track = std::make_shared<std::map<uint32_t, iso::TrackFragmentInfo>>();
  return c;
}

void add_uint(BoxNodeT& n, std::string_view name, uint64_t v, uint64_t bit, uint32_t len) {
  auto f = std::make_unique<FieldT>();
  f->name = std::string(name);
  f->kind = ovt::fb::SyntaxValueKind::UInt;
  f->uvalue = v;
  f->span = std::make_unique<ovt::fb::BitSpan>(bit, len);
  n.fields.push_back(std::move(f));
}

struct Decoded {
  BoxNodeT node;
  iso::DecodeOutcome outcome;
};

// Runs the registered decoder of `type` the way the walker does: FullBox version/flags first (all group E decoders
// are FullBoxes).
Decoded run(std::string_view type, std::string_view parent, const Bytes& payload, iso::BoxContext& ctx) {
  Decoded d;
  d.node.fourcc = std::string(type);
  d.node.header_length = 8;
  d.node.file_span = std::make_unique<ovt::fb::ByteSpan>(0, 8 + payload.size());
  const iso::RegistryEntry* e = iso::lookup(type, ctx);
  REQUIRE(e != nullptr);
  REQUIRE(e->decoder != nullptr);
  REQUIRE(payload.size() >= 4);
  d.node.version = payload[0];
  d.node.flags = static_cast<int32_t>((uint32_t{payload[1]} << 16U) | (uint32_t{payload[2]} << 8U) | payload[3]);
  add_uint(d.node, "version", payload[0], 64, 8);
  add_uint(d.node, "flags", static_cast<uint32_t>(d.node.flags), 72, 24);
  ovt::BitReader br(std::span<const uint8_t>(payload).subspan(4));
  iso::FieldBuilder fb(d.node, 0, br, 96);
  d.outcome = iso::run_decoder(*e->decoder, ctx, br, fb);
  static_cast<void>(parent);
  return d;
}

const FieldT* child(const std::vector<std::unique_ptr<FieldT>>& fields, std::string_view name) {
  for (const auto& f : fields) {
    if (f->name == name) {
      return f.get();
    }
  }
  return nullptr;
}

const FieldT& field(const BoxNodeT& n, std::string_view path) {
  const std::vector<std::unique_ptr<FieldT>>* level = &n.fields;
  const FieldT* f = nullptr;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    f = child(*level, path.substr(0, slash));
    REQUIRE(f != nullptr);
    level = &f->children;
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
  }
  REQUIRE(f != nullptr);
  return *f;
}

bool has_code(const BoxNodeT& n, std::string_view code) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code; });
}

// sample_flags: sync sample, depends_on 2 (not I), priority 7.
constexpr uint32_t kSync = 0x02000007;
constexpr uint32_t kNonSync = 0x01010000;

Bytes trex_body(uint32_t track, uint32_t duration, uint32_t size, uint32_t flags) {
  return cat({be(track, 4), be(1, 4), be(duration, 4), be(size, 4), be(flags, 4)});
}

}  // namespace

TEST_CASE("trex fills the shared track table and its flags decode into sub-fields", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("mvex");
  const Decoded d = run("trex", "mvex", full(0, 0, trex_body(201, 1000, 0, kNonSync)), ctx);
  CHECK(field(d.node, "track_ID").uvalue == 201);
  CHECK(field(d.node, "default_sample_duration").uvalue == 1000);
  CHECK(field(d.node, "default_sample_flags").uvalue == kNonSync);
  CHECK(field(d.node, "default_sample_flags.bits/is_leading").uvalue == 0);
  CHECK(field(d.node, "default_sample_flags.bits/sample_depends_on").uvalue == 1);
  CHECK(field(d.node, "default_sample_flags.bits/sample_is_non_sync_sample").uvalue == 1);
  // The bits repeat the leaf's span: 4+2+2+2+2+3+1+16 = 32 bits.
  CHECK(field(d.node, "default_sample_flags.bits").span->bit_length() == 32);
  CHECK(field(d.node, "default_sample_flags.bits/sample_is_non_sync_sample").span->bit_offset() ==
        field(d.node, "default_sample_flags").span->bit_offset() + 15);
  REQUIRE(ctx.trex_for_track->contains(201));
  CHECK(ctx.trex_for_track->at(201).default_sample_duration == 1000);
  CHECK(ctx.trex_for_track->at(201).has_trex);
  const Decoded again = run("trex", "mvex", full(0, 0, trex_body(201, 1, 1, 0)), ctx);
  CHECK(has_code(again.node, "CNT-1012"));
  CHECK(ctx.trex_for_track->at(201).default_sample_duration == 1000);
}

TEST_CASE("mehd, mfhd and leva", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("mvex");
  ctx.movie_timescale = 1000;
  const Decoded m0 = run("mehd", "mvex", full(0, 0, be(2500, 4)), ctx);
  CHECK(field(m0.node, "fragment_duration").uvalue == 2500);
  CHECK(field(m0.node, "fragment_duration").svalue == "2.500 s");
  const Decoded m1 = run("mehd", "mvex", full(1, 0, be(5000000000ULL, 8)), ctx);
  CHECK(field(m1.node, "fragment_duration").uvalue == 5000000000ULL);

  iso::BoxContext mctx = ctx_in("moof");
  const Decoded mf = run("mfhd", "moof", full(0, 0, be(7, 4)), mctx);
  CHECK(field(mf.node, "sequence_number").uvalue == 7);
  CHECK_FALSE(has_code(mf.node, "CNT-4001"));  // the timeline builder checks monotonicity

  const Decoded lv = run("leva", "mvex",
                         full(0, 0,
                              cat({be(3, 1), be(1, 4), be(0x80, 1), be(0x73676270, 4),  // level 0: 'sgbp' grouping
                                   be(2, 4), be(1, 1), be(0x73676270, 4), be(5, 4),     // level 1: with parameter
                                   be(3, 4), be(4, 1), be(9, 4)})),                     // level 2: sub-track

                         ctx);
  CHECK(field(lv.node, "level_count").uvalue == 3);
  CHECK(field(lv.node, "levels/level[0]/padding_flag").uvalue == 1);
  CHECK(field(lv.node, "levels/level[0]/assignment_type").svalue == "sample grouping");
  CHECK(field(lv.node, "levels/level[1]/grouping_type_parameter").uvalue == 5);
  CHECK(field(lv.node, "levels/level[2]/sub_track_id").uvalue == 9);
}

TEST_CASE("tfhd reads the flag-driven fields and derives the effective defaults", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  const Decoded t = run("trex", "mvex", full(0, 0, trex_body(5, 1000, 400, kNonSync)), ctx);
  static_cast<void>(t);
  const Decoded d =
      run("tfhd", "traf", full(0, 0x020000 | 0x8 | 0x20, cat({be(5, 4), be(3000, 4), be(kSync, 4)})), ctx);
  CHECK(field(d.node, "flags").svalue ==
        "default-sample-duration-present|default-sample-flags-present|default-base-is-moof");
  CHECK(field(d.node, "track_ID").uvalue == 5);
  CHECK(field(d.node, "default_sample_duration").uvalue == 3000);
  CHECK((field(d.node, "flags").uvalue & 0x020000U) != 0);
  CHECK(ctx.track_id == 5);
  const std::string& eff = field(d.node, "effective_defaults").svalue;
  CHECK(eff.find("duration=3000 (tfhd)") != std::string::npos);
  CHECK(eff.find("size=400 (trex)") != std::string::npos);
  CHECK(eff.find("flags=0x02000007 (tfhd)") != std::string::npos);
  CHECK_FALSE(has_code(d.node, "CNT-4002"));

  iso::BoxContext bare = ctx_in("traf");
  const Decoded both = run("tfhd", "traf", full(0, 0x020001, cat({be(9, 4), be(0x1000, 8)})), bare);
  CHECK(field(both.node, "base_data_offset").uvalue == 0x1000);
  CHECK(has_code(both.node, "CNT-4002"));
  CHECK(field(both.node, "effective_defaults").svalue.find("duration=none") != std::string::npos);
}

TEST_CASE("tfdt v0 and v1 with the track timescale from mdhd", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  (*ctx.trex_for_track)[3].timescale = 90000;
  ctx.track_id = 3;
  const Decoded v0 = run("tfdt", "traf", full(0, 0, be(180000, 4)), ctx);
  CHECK(field(v0.node, "baseMediaDecodeTime").uvalue == 180000);
  CHECK(field(v0.node, "baseMediaDecodeTime").svalue == "2.000 s");
  const Decoded v1 = run("tfdt", "traf", full(1, 0, be(0x100000000ULL, 8)), ctx);
  CHECK(field(v1.node, "baseMediaDecodeTime").uvalue == 0x100000000ULL);
}

TEST_CASE("trun v0 with every flag, in the fixed field order", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  constexpr uint32_t flags = 0x1 | 0x100 | 0x200 | 0x400 | 0x800;
  const Bytes body = cat({be(2, 4), be(0x80, 4),                                          // sample_count, data_offset
                          be(40, 4), be(1000, 4), be(kSync, 4), be(2000, 4),              // sample 0
                          be(41, 4), be(1500, 4), be(kNonSync, 4), be(0xFFFFFFFFU, 4)});  // sample 1
  const Decoded d = run("trun", "traf", full(0, flags, body), ctx);
  CHECK(field(d.node, "sample_count").uvalue == 2);
  CHECK(field(d.node, "data_offset").ivalue == 0x80);
  CHECK(field(d.node, "entries/entry[0]/sample_duration").uvalue == 40);
  CHECK(field(d.node, "entries/entry[0]/sample_size").uvalue == 1000);
  CHECK(field(d.node, "entries/entry[0]/sample_flags.bits/sample_is_non_sync_sample").uvalue == 0);
  CHECK(field(d.node, "entries/entry[1]/sample_flags.bits/sample_is_non_sync_sample").uvalue == 1);
  CHECK(field(d.node, "entries/entry[1]/sample_composition_time_offset").uvalue == 0xFFFFFFFFU);  // v0: unsigned
  CHECK(field(d.node, "summary").svalue ==
        "sum_sample_duration=81 sum_sample_size=2500 sync_samples=1 of 2 with known flags");
  CHECK_FALSE(has_code(d.node, "CNT-1005"));
}

TEST_CASE("trun v1 composition offsets can be negative", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  const Bytes body = cat({be(2, 4), be(static_cast<uint32_t>(-2000), 4), be(3000, 4)});
  const Decoded d = run("trun", "traf", full(1, 0x800, body), ctx);
  CHECK(field(d.node, "entries/entry[0]/sample_composition_time_offset").ivalue == -2000);
  CHECK(field(d.node, "entries/entry[1]/sample_composition_time_offset").ivalue == 3000);
}

TEST_CASE("trun first_sample_flags applies to sample 0 only; both flags raise CNT-4003",
          "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  const Decoded d =
      run("trun", "traf", full(0, 0x4 | 0x200, cat({be(3, 4), be(kSync, 4), be(10, 4), be(11, 4), be(12, 4)})), ctx);
  CHECK(field(d.node, "first_sample_flags").uvalue == kSync);
  CHECK(field(d.node, "first_sample_flags.bits/sample_is_non_sync_sample").uvalue == 0);
  CHECK(field(d.node, "summary").svalue.find("sync_samples=1 of 1") != std::string::npos);
  CHECK_FALSE(has_code(d.node, "CNT-4003"));

  const Decoded both = run("trun", "traf", full(0, 0x4 | 0x400, cat({be(1, 4), be(kSync, 4), be(kNonSync, 4)})), ctx);
  CHECK(has_code(both.node, "CNT-4003"));
  CHECK(field(both.node, "entries/entry[0]/sample_flags").uvalue == kNonSync);
}

TEST_CASE("trun with a declared count beyond the box raises CNT-1014 and keeps the samples that exist",
          "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  const Decoded d = run("trun", "traf", full(0, 0x200, cat({be(1000, 4), be(7, 4), be(8, 4)})), ctx);
  CHECK(has_code(d.node, "CNT-1014"));
  CHECK(field(d.node, "sample_count").uvalue == 1000);
  CHECK(field(d.node, "entries").uvalue == 2);
  CHECK(field(d.node, "summary").svalue.find("sum_sample_size=15") != std::string::npos);
}

TEST_CASE("trun without per-sample flags never loops over a huge sample_count", "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  const Decoded d = run("trun", "traf", full(0, 0, be(0xFFFFFFFFU, 4)), ctx);
  CHECK(field(d.node, "sample_count").uvalue == 0xFFFFFFFFU);
  CHECK_FALSE(has_code(d.node, "CNT-1014"));
}

TEST_CASE("a large trun materialises the first 64 and the last 16 entries and sums all of them",
          "[container][decoders][moof]") {
  iso::BoxContext ctx = ctx_in("traf");
  constexpr uint32_t n = 5000;
  Bytes body = be(n, 4);
  for (uint32_t i = 0; i < n; ++i) {
    const Bytes v = be(i, 4);
    body.insert(body.end(), v.begin(), v.end());
  }
  const Decoded d = run("trun", "traf", full(0, 0x100, body), ctx);
  const FieldT& entries = field(d.node, "entries");
  CHECK(entries.uvalue == n);
  CHECK(entries.svalue == "see Sample Table view");
  CHECK(entries.children.size() == 64 + 16);
  CHECK(field(d.node, "entries/entry[4999]/sample_duration").uvalue == 4999);
  CHECK(child(entries.children, "entry[100]") == nullptr);
  CHECK(field(d.node, "summary").svalue.find("sum_sample_duration=12497500") != std::string::npos);
}

TEST_CASE("every group E decoder survives truncation at every length", "[container][decoders][moof]") {
  const std::vector<std::pair<std::string_view, Bytes>> boxes = {
      {"trex", full(0, 0, trex_body(1, 2, 3, kSync))},
      {"tfhd", full(0, 0x3B, cat({be(1, 4), be(2, 8), be(1, 4), be(2, 4), be(3, 4), be(kSync, 4)}))},
      {"trun", full(1, 0xF05,
                    cat({be(2, 4), be(8, 4), be(kSync, 4), be(1, 4), be(2, 4), be(kSync, 4), be(3, 4), be(4, 4),
                         be(5, 4), be(kSync, 4), be(6, 4)}))},
      {"leva", full(0, 0, cat({be(1, 1), be(1, 4), be(1, 1), be(1, 4), be(1, 4)}))},
  };
  for (const auto& [type, payload] : boxes) {
    for (size_t len = 4; len < payload.size(); ++len) {
      iso::BoxContext ctx = ctx_in("traf");
      const Bytes cut(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(len));
      INFO(std::string(type) + " " + std::to_string(len));
      CHECK_NOTHROW(run(type, "traf", cut, ctx));
    }
  }
}

// ---- through the walker ----

namespace {

ovt::fb::BoxNodeT* find(std::vector<std::unique_ptr<BoxNodeT>>& boxes, std::string_view type) {
  for (auto& b : boxes) {
    if (b->fourcc == type) {
      return b.get();
    }
  }
  return nullptr;
}

iso::WalkResult walk_bytes(const Bytes& data) {
  const ovt::io::MemoryFile file(data);
  return iso::walk(file);
}

}  // namespace

TEST_CASE("the walker connects trex, tfhd and the moof description", "[container][decoders][moof]") {
  const Bytes mvex = box("mvex", box("trex", full(0, 0, trex_body(1, 1000, 77, kSync))));
  const Bytes tfhd = box("tfhd", full(0, 0x020000, be(1, 4)));
  const Bytes trun = box("trun", full(0, 0x200, cat({be(1, 4), be(5, 4)})));
  const Bytes moof = box("moof", cat({box("mfhd", full(0, 0, be(42, 4))), box("traf", cat({tfhd, trun}))}));
  const Bytes data = cat({box("ftyp", cat({Bytes{'i', 's', 'o', 'm'}, be(0, 4)})), box("moov", mvex), moof});
  iso::WalkResult w = walk_bytes(data);
  BoxNodeT* mf = find(w.top_level, "moof");
  REQUIRE(mf != nullptr);
  CHECK(mf->description == "movie fragment, sequence_number 42");
  BoxNodeT* traf = find(mf->children, "traf");
  REQUIRE(traf != nullptr);
  BoxNodeT* th = find(traf->children, "tfhd");
  REQUIRE(th != nullptr);
  CHECK(field(*th, "effective_defaults").svalue.find("size=77 (trex)") != std::string::npos);
  CHECK_FALSE(has_code(*traf, "CNT-1011"));
}

TEST_CASE("a traf without tfhd raises CNT-1011 naming tfhd", "[container][decoders][moof]") {
  const Bytes moof =
      box("moof", cat({box("mfhd", full(0, 0, be(1, 4))), box("traf", box("tfdt", full(0, 0, be(0, 4))))}));
  const Bytes data = cat({box("ftyp", cat({Bytes{'i', 's', 'o', 'm'}, be(0, 4)})), moof});
  iso::WalkResult w = walk_bytes(data);
  BoxNodeT* mf = find(w.top_level, "moof");
  REQUIRE(mf != nullptr);
  BoxNodeT* traf = find(mf->children, "traf");
  REQUIRE(traf != nullptr);
  REQUIRE(has_code(*traf, "CNT-1011"));
  const auto it = std::ranges::find_if(traf->diagnostics, [](const auto& d) { return d->code == "CNT-1011"; });
  CHECK((*it)->message.find("tfhd") != std::string::npos);
  CHECK((*it)->severity == ovt::fb::Severity::Error);
  iso::check_fragment_children(*traf);  // idempotent
  CHECK(std::ranges::count_if(traf->diagnostics, [](const auto& d) { return d->code == "CNT-1011"; }) == 1);
}

// ---- corpus checks ----

namespace {

nlohmann::json json_of(const FieldT& f);

nlohmann::json json_of_children(const std::vector<std::unique_ptr<FieldT>>& children) {
  const bool indexed = !children.empty() && std::ranges::all_of(children, [](const auto& c) {
    return !c->name.empty() && c->name.back() == ']';
  });
  if (indexed) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& c : children) {
      a.push_back(json_of(*c));
    }
    return a;
  }
  nlohmann::json o = nlohmann::json::object();
  for (const auto& c : children) {
    o[c->name] = json_of(*c);
  }
  return o;
}

nlohmann::json json_of(const FieldT& f) {
  if (!f.children.empty()) {
    return json_of_children(f.children);
  }
  switch (f.kind) {
    case ovt::fb::SyntaxValueKind::Int:
    case ovt::fb::SyntaxValueKind::Fixed:
      return f.ivalue;
    case ovt::fb::SyntaxValueKind::String:
      return f.svalue;
    default:
      return f.uvalue;
  }
}

struct Harness {
  std::map<std::string, nlohmann::json> by_path;
  std::map<std::string, const BoxNodeT*> nodes;

  void visit(const std::vector<std::unique_ptr<BoxNodeT>>& boxes, const std::string& parent) {
    std::map<std::string, int> total;
    for (const auto& b : boxes) {
      ++total[b->fourcc];
    }
    std::map<std::string, int> seen;
    for (const auto& b : boxes) {
      std::string path = parent.empty() ? b->fourcc : parent + "/" + b->fourcc;
      if (total[b->fourcc] > 1) {
        path += "[" + std::to_string(++seen[b->fourcc]) + "]";
      }
      if (!b->fields.empty()) {
        by_path[path] = json_of_children(b->fields);
      }
      nodes[path] = b.get();
      visit(b->children, path);
    }
  }
};

const std::array<std::string_view, 6> kGroupE = {"mehd", "trex", "mfhd", "tfhd", "tfdt", "trun"};

std::string last_type(const std::string& path) {
  const std::string last = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
  return last.substr(0, last.find('['));
}

size_t compare_with_golden(std::string_view id) {
  const auto path = ovt::test::corpus_or_skip(id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult walked = iso::walk(*file);
  Harness h;
  h.visit(walked.top_level, "");
  std::ifstream gf(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/isobmff" /
                   (std::string(id) + ".mp4box-diso.json"));
  REQUIRE(gf.good());
  const auto golden = nlohmann::json::parse(gf);
  size_t compared = 0;
  for (const auto& b : golden) {
    const std::string p = b.at("path").get<std::string>();
    if (std::ranges::find(kGroupE, last_type(p)) == kGroupE.end() || !b.contains("fields")) {
      continue;
    }
    if (b.at("fields").contains("sample_count") && b.at("fields").at("sample_count").get<uint64_t>() > 1024) {
      continue;  // large arrays are summarised (§8.2.5), so the element lists differ by design
    }
    INFO(std::string(id) + " " + p);
    REQUIRE(h.by_path.contains(p));
    const auto diffs = ovt::test::compare_canonical(b.at("fields"), h.by_path.at(p));
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d.path + ": " + d.message);
    }
    CHECK(diffs.empty());
    ++compared;
  }
  return compared;
}

}  // namespace

TEST_CASE("group E fields match the MP4Box goldens of the container-smoke corpus",
          "[container][decoders][moof][golden][needs-corpus]") {
  for (const std::string_view id : {"mpegff_10_fragments", "mpegff_a4_tone_fragmented",
                                    "mpegff_fragment_random_access_2", "mpegff_video_cenc_roll_frag1s"}) {
    // Outside CHECK: a missing corpus file SKIPs the test case, and a SKIP inside a CHECK expression is a failure.
    const size_t compared = compare_with_golden(id);
    CHECK(compared > 0);
  }
}

TEST_CASE("the first moof of cta_cmaf_video_cmfv", "[container][decoders][moof][slow][needs-corpus]") {
  // Acceptance test 1. SPEC-ASSUMPTION(#671): the repository has no mp4box.js golden for this slow-tagged file, so
  // the values are checked against what the file itself implies: with default-base-is-moof the first sample starts
  // where the following mdat payload does.
  const auto path = ovt::test::corpus_or_skip("cta_cmaf_video_cmfv");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  iso::WalkResult w = iso::walk(*file);
  Harness h;
  h.visit(w.top_level, "");
  const std::string moof = h.nodes.contains("moof[1]") ? "moof[1]" : "moof";
  REQUIRE(h.nodes.contains(moof));
  const std::string traf = h.nodes.contains(moof + "/traf[1]") ? moof + "/traf[1]" : moof + "/traf";
  const BoxNodeT& tfhd = *h.nodes.at(traf + "/tfhd");
  CHECK((field(tfhd, "flags").uvalue & 0x020000U) != 0);
  const std::string trun_path = h.nodes.contains(traf + "/trun[1]") ? traf + "/trun[1]" : traf + "/trun";
  const BoxNodeT& trun = *h.nodes.at(trun_path);
  CHECK(field(trun, "sample_count").uvalue > 0);
  const BoxNodeT* mdat = nullptr;
  for (const auto& b : w.top_level) {
    if (b->fourcc == "mdat" && b->file_span->offset() > h.nodes.at(moof)->file_span->offset()) {
      mdat = b.get();
      break;
    }
  }
  REQUIRE(mdat != nullptr);
  const auto first = static_cast<int64_t>(h.nodes.at(moof)->file_span->offset()) + field(trun, "data_offset").ivalue;
  CHECK(first == static_cast<int64_t>(mdat->file_span->offset() + mdat->header_length));
  const bool has_first = child(trun.fields, "first_sample_flags") != nullptr;
  const FieldT& flags = has_first ? field(trun, "first_sample_flags.bits/sample_is_non_sync_sample")
                                  : field(trun, "entries/entry[0]/sample_flags.bits/sample_is_non_sync_sample");
  CHECK(flags.uvalue == 0);
}
