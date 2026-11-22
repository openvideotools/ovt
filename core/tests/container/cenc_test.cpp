// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the CENC per-sample encryption map and the clear / protected ranges (chapter 08 §8.4.4, WI-1.221), on
// synthetic files built in memory (`saio`/`saiz` aux data of a `moov` track, `cenc` and `cbcs` signalling, `seig`
// overrides, the diagnostics) and, tagged [needs-corpus], on the real files of AT1 and AT2.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/isobmff/cenc.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using Range = ovt::syntax::ByteRange;

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
Bytes str(std::string_view s) {
  return {s.begin(), s.end()};
}
Bytes filled(size_t n, uint8_t v) {
  return Bytes(n, v);
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  return cat({be(8 + payload.size(), 4), str(type), payload});
}
Bytes full(uint8_t version, uint32_t flags, const Bytes& body) {
  return cat({be(version, 1), be(flags, 3), body});
}

constexpr uint32_t kTrack = 1;
const Bytes kKid = [] {
  Bytes k;
  for (uint8_t i = 0; i < 16; ++i) {
    k.push_back(static_cast<uint8_t>(0xA0 + i));
  }
  return k;
}();

Bytes tenc_v0(uint8_t is_protected, uint8_t iv_size) {
  return box("tenc", full(0, 0, cat({be(0, 2), be(is_protected, 1), be(iv_size, 1), kKid})));
}
Bytes tenc_cbcs(uint8_t crypt, uint8_t skip, const Bytes& constant_iv) {
  return box("tenc", full(1, 0,
                          cat({be(0, 1), be(static_cast<uint64_t>(crypt << 4U) | skip, 1), be(1, 1), be(0, 1), kKid,
                               be(constant_iv.size(), 1), constant_iv})));
}

Bytes ftyp() {
  return box("ftyp", cat({str("isom"), be(0, 4), str("isom"), str("cmfc")}));
}

// An encrypted AVC entry with a 4-byte NAL length field.
Bytes encv(std::string_view scheme, const Bytes& tenc_box) {
  const Bytes avcc = box("avcC", Bytes{1, 100, 0, 30, 0xFF, 0xE0, 0});
  const Bytes sinf =
      box("sinf", cat({box("frma", str("avc1")), box("schm", full(0, 0, cat({str(scheme), be(0x10000, 4)}))),
                       box("schi", tenc_box)}));
  return box("encv", cat({filled(78, 0), avcc, sinf}));
}

Bytes trak(const Bytes& entry, const Bytes& tables) {
  const Bytes tkhd = box("tkhd", full(0, 3,
                                      cat({be(0, 4), be(0, 4), be(kTrack, 4), be(0, 4), be(0, 4), filled(8, 0),
                                           be(0, 4), be(0, 4), filled(36, 0), be(0, 4), be(0, 4)})));
  const Bytes mdhd = box("mdhd", full(0, 0, cat({be(0, 8), be(1000, 4), be(0, 8)})));
  const Bytes hdlr = box("hdlr", full(0, 0, cat({be(0, 4), str("vide"), filled(12, 0), Bytes{0}})));
  const Bytes stsd = box("stsd", full(0, 0, cat({be(1, 4), entry})));
  return box("trak", cat({tkhd, box("mdia", cat({mdhd, hdlr, box("minf", box("stbl", cat({stsd, tables})))}))}));
}

Bytes empty_tables() {
  return cat({box("stts", full(0, 0, be(0, 4))), box("stsc", full(0, 0, be(0, 4))),
              box("stsz", full(0, 0, cat({be(0, 4), be(0, 4)}))), box("stco", full(0, 0, be(0, 4)))});
}

Bytes trex() {
  return box("mvex", box("trex", full(0, 0, cat({be(kTrack, 4), be(1, 4), be(40, 4), be(0, 4), be(0, 4)}))));
}

// ---- the fixtures ----

using Sub = std::pair<uint16_t, uint32_t>;

struct SampleSpec {
  Bytes iv;
  std::vector<Sub> subs;
  uint32_t size = 0;
};

Bytes record(const SampleSpec& s, bool with_subs) {
  Bytes b = s.iv;
  if (with_subs) {
    b = cat({b, be(s.subs.size(), 2)});
    for (const auto& [clear, prot] : s.subs) {
      b = cat({b, be(clear, 2), be(prot, 4)});
    }
  }
  return b;
}

// A length-prefixed one-NAL sample of `size` bytes (4-byte length, header byte 0x65).
Bytes nal_sample(uint32_t size) {
  Bytes b = cat({be(size - 4, 4), Bytes{0x65}, filled(size - 5, 0x11)});
  return b;
}

// A file with the samples and their saiz/saio data in the moov (the track has no fragments).
struct MoovSpec {
  std::string scheme = "cenc";
  Bytes tenc = tenc_v0(1, 8);
  std::vector<SampleSpec> samples;
  Bytes extra_tables;                  // sgpd / sbgp of the stbl
  std::optional<uint32_t> saiz_count;  // overrides the sample_count written to saiz
  bool with_aux = true;
};

Bytes moov_file(const MoovSpec& m) {
  Bytes mdat_payload;
  Bytes sizes;
  Bytes aux;
  for (const auto& s : m.samples) {
    mdat_payload = cat({mdat_payload, nal_sample(s.size)});
    const Bytes r = record(s, true);
    aux = cat({aux, r});
    sizes.push_back(static_cast<uint8_t>(r.size()));
  }
  const auto build = [&](uint64_t mdat_at) {
    Bytes stsz = full(0, 0, cat({be(0, 4), be(m.samples.size(), 4)}));
    for (const auto& s : m.samples) {
      stsz = cat({stsz, be(s.size, 4)});
    }
    const uint64_t saiz_count = m.saiz_count.value_or(static_cast<uint32_t>(m.samples.size()));
    const Bytes aux_boxes =
        m.with_aux ? cat({box("saiz", full(0, 0, cat({be(0, 1), be(saiz_count, 4), sizes}))),
                          box("saio", full(0, 0, cat({be(1, 4), be(mdat_at + 8 + mdat_payload.size(), 4)})))})
                   : Bytes{};
    const Bytes tables =
        cat({box("stts", full(0, 0, cat({be(1, 4), be(m.samples.size(), 4), be(40, 4)}))),
             box("stsc", full(0, 0, cat({be(1, 4), be(1, 4), be(m.samples.size(), 4), be(1, 4)}))), box("stsz", stsz),
             box("stco", full(0, 0, cat({be(1, 4), be(mdat_at + 8, 4)}))), aux_boxes, m.extra_tables});
    return box("moov", trak(encv(m.scheme, m.tenc), tables));
  };
  const uint64_t mdat_at = ftyp().size() + build(0).size();
  return cat({ftyp(), build(mdat_at), box("mdat", cat({mdat_payload, aux}))});
}

struct Opened {
  ovt_File handle;
  ovt_Container* c = nullptr;
  explicit Opened(const Bytes& bytes) : handle{std::make_unique<ovt::io::MemoryFile>(bytes)} {
    REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  Opened(Opened&&) = delete;
  Opened& operator=(Opened&&) = delete;
  ~Opened() { ovt_container_close(c); }
};

std::unique_ptr<ovt::fb::EncryptionInfoT> encryption_of(ovt_Container* c, uint64_t first = 0, uint64_t count = 0) {
  ovt_Buffer b{};
  REQUIRE(ovt_container_encryption(c, 0, first, count, &b) == OVT_OK);
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::EncryptionInfo>(nullptr));
  auto info =
      std::unique_ptr<ovt::fb::EncryptionInfoT>(flatbuffers::GetRoot<ovt::fb::EncryptionInfo>(b.data)->UnPack());
  delete[] b.data;  // §12.3.1 rule 3a
  return info;
}

std::vector<std::string> codes(const ovt::fb::EncryptionInfoT& info) {
  std::vector<std::string> out;
  out.reserve(info.diagnostics.size());
  for (const auto& d : info.diagnostics) {
    out.push_back(d->code);
  }
  return out;
}
bool has(const ovt::fb::EncryptionInfoT& info, std::string_view code) {
  const auto c = codes(info);
  return std::ranges::find(c, code) != c.end();
}

uint64_t sum_of(const ovt::fb::SampleEncryptionT& s) {
  uint64_t t = 0;
  for (const auto& r : s.subsamples) {
    t += uint64_t{r->clear_bytes} + r->protected_bytes;
  }
  return t;
}

}  // namespace

TEST_CASE("cenc: a range of samples is built on request", "[container][cenc]") {
  MoovSpec m;
  m.samples = {{filled(8, 1), {{5, 59}}, 64}, {filled(8, 2), {{5, 59}}, 64}, {filled(8, 3), {{5, 59}}, 64}};
  const Opened o(moov_file(m));
  const auto mid = encryption_of(o.c, 1, 1);
  REQUIRE(mid->samples.size() == 1);
  CHECK(mid->samples[0]->sample_index == 1);
  CHECK(mid->samples[0]->iv == filled(8, 2));
  CHECK(encryption_of(o.c, 3, 5)->samples.empty());
  CHECK(encryption_of(o.c, 2, 99)->samples.size() == 1);
}

TEST_CASE("cenc: a subsample that protects the NAL length and header raises CNT-6007", "[container][cenc]") {
  // sample 0: the first subsample of the NAL keeps its 5 header bytes clear, the continuation starts inside the NAL
  // with BytesOfClearData == 0 (legal); sample 1: the first subsample protects byte 0 of the NAL.
  MoovSpec m;
  m.samples = {{filled(8, 1), {{5, 20}, {0, 39}}, 64}, {filled(8, 2), {{0, 64}}, 64}};
  const Opened o(moov_file(m));
  const auto info = encryption_of(o.c);
  size_t n = 0;
  for (const auto& d : info->diagnostics) {
    if (d->code == "CNT-6007") {
      ++n;
      CHECK(d->message.find("sample 1") != std::string::npos);
    }
  }
  CHECK(n == 1);
}

TEST_CASE("cenc: a seig entry of the stbl makes a sample unprotected", "[container][cenc]") {
  // sgpd v1 (default_length 20) with one entry isProtected 0, and an sbgp mapping sample 1 to it.
  const Bytes seig_entry = cat({be(0, 1), be(0, 1), be(0, 1), be(0, 1), kKid});
  const Bytes sgpd = box("sgpd", full(1, 0, cat({str("seig"), be(20, 4), be(1, 4), seig_entry})));
  const Bytes sbgp = box("sbgp", full(0, 0, cat({str("seig"), be(2, 4), be(1, 4), be(0, 4), be(1, 4), be(1, 4)})));
  MoovSpec m;
  m.samples = {{filled(8, 5), {{5, 59}}, 64}, {{}, {}, 64}};
  m.extra_tables = cat({sgpd, sbgp});
  // the unprotected sample has no auxiliary data: its saiz size is 0
  const Opened o(moov_file(m));
  const auto info = encryption_of(o.c);
  REQUIRE(info->samples.size() == 2);
  CHECK(info->samples[0]->is_protected);
  CHECK(info->samples[0]->iv == filled(8, 5));
  CHECK_FALSE(info->samples[1]->is_protected);
  CHECK(info->samples[1]->iv.empty());
  CHECK(info->samples[1]->subsamples.empty());
}

TEST_CASE("cenc: findings", "[container][cenc]") {
  MoovSpec m;
  SECTION("a subsample sum that differs from the sample size (CNT-6002)") {
    m.samples = {{filled(8, 1), {{5, 50}}, 64}};
    const Opened o(moov_file(m));
    const auto info = encryption_of(o.c);
    CHECK(has(*info, "CNT-6002"));
    CHECK(sum_of(*info->samples[0]) == 55);
  }
  SECTION("fewer saiz entries than samples (CNT-6015)") {
    m.samples = {{filled(8, 1), {{5, 59}}, 64}, {filled(8, 2), {{5, 59}}, 64}};
    m.saiz_count = 1;
    const Opened o(moov_file(m));
    const auto info = encryption_of(o.c);
    REQUIRE(info->samples.size() == 2);
    CHECK(info->samples[0]->is_protected);
    CHECK_FALSE(info->samples[1]->is_protected);
    CHECK(has(*info, "CNT-6015"));
  }
  SECTION("a track whose tenc says unprotected and has no seig (CNT-6006)") {
    m.tenc = tenc_v0(0, 0);
    m.with_aux = false;
    m.samples = {{{}, {}, 64}};
    const Opened o(moov_file(m));
    const auto info = encryption_of(o.c);
    CHECK(has(*info, "CNT-6006"));
    REQUIRE(info->samples.size() == 1);
    CHECK_FALSE(info->samples[0]->is_protected);
  }
  SECTION("cbcs with per-sample IVs (CNT-6008)") {
    m.scheme = "cbcs";
    m.samples = {{filled(8, 1), {{5, 59}}, 64}};
    const Opened o(moov_file(m));
    CHECK(has(*encryption_of(o.c), "CNT-6008"));
  }
  SECTION("a bounded list of findings per code") {
    for (int i = 0; i < 40; ++i) {
      m.samples.push_back({filled(8, 1), {{5, 50}}, 64});
    }
    const Opened o(moov_file(m));
    const auto info = encryption_of(o.c);
    size_t n = 0;
    for (const auto& d : info->diagnostics) {
      n += d->code == "CNT-6002" ? 1U : 0U;
    }
    CHECK(n == iso::kMaxListedCencFindings + 1);  // the listed ones and one summary
  }
}

TEST_CASE("cenc: a track without encryption has no scheme and no samples", "[container][cenc]") {
  const Bytes plain_entry = box("avc1", filled(78, 0));
  const Bytes moov = box("moov", cat({trak(plain_entry, empty_tables()), trex()}));
  const Opened o(cat({ftyp(), moov}));
  const auto info = encryption_of(o.c);
  CHECK(info->scheme_type.empty());
  CHECK(info->samples.empty());
}

TEST_CASE("cenc: sample_clear_protected_ranges", "[container][cenc]") {
  ovt::fb::SampleEncryptionT s;
  s.is_protected = true;
  s.scheme = "cenc";
  const auto lengths = [](const std::vector<Range>& r) {
    std::vector<uint64_t> out;
    out.reserve(r.size());
    for (const auto& x : r) {
      out.push_back(x.length);
    }
    return out;
  };
  SECTION("not protected") {
    s.is_protected = false;
    const auto r = iso::sample_clear_protected_ranges(s, 100, 50);
    REQUIRE(r.size() == 1);
    CHECK(r[0].offset == 100);
    CHECK(r[0].length == 50);
  }
  SECTION("protected without subsamples is one protected range after an empty clear one") {
    const auto r = iso::sample_clear_protected_ranges(s, 100, 50);
    CHECK(lengths(r) == std::vector<uint64_t>{0, 50});
  }
  SECTION("subsamples: clear, protected, clear, protected") {
    for (const auto& [c, p] : std::vector<Sub>{{10, 20}, {5, 15}}) {
      auto sub = std::make_unique<ovt::fb::SubsampleRangeT>();
      sub->clear_bytes = c;
      sub->protected_bytes = p;
      s.subsamples.push_back(std::move(sub));
    }
    const auto r = iso::sample_clear_protected_ranges(s, 0, 50);
    CHECK(lengths(r) == std::vector<uint64_t>{10, 20, 5, 15});
    CHECK(r[2].offset == 30);
  }
  SECTION("cbc1 leaves the partial trailing block clear, cenc does not") {
    auto sub = std::make_unique<ovt::fb::SubsampleRangeT>();
    sub->clear_bytes = 4;
    sub->protected_bytes = 40;
    s.subsamples.push_back(std::move(sub));
    CHECK(lengths(iso::sample_clear_protected_ranges(s, 0, 44)) == std::vector<uint64_t>{4, 40});
    s.scheme = "cbc1";
    CHECK(lengths(iso::sample_clear_protected_ranges(s, 0, 44)) == std::vector<uint64_t>{4, 32, 8});
  }
  SECTION("cens 1:2 restarts at the protected range and encrypts a partial block inside a crypt run") {
    s.scheme = "cens";
    s.crypt_byte_block = 1;
    s.skip_byte_block = 2;
    auto sub = std::make_unique<ovt::fb::SubsampleRangeT>();
    sub->clear_bytes = 0;
    sub->protected_bytes = 16 * 3 + 5;  // block 0 is crypt, blocks 1-2 skip, the partial block 3 starts a new cycle
    s.subsamples.push_back(std::move(sub));
    CHECK(lengths(iso::sample_clear_protected_ranges(s, 0, 53)) == std::vector<uint64_t>{0, 16, 32, 5});
  }
  SECTION("cbcs 1:2 leaves the partial block clear") {
    s.scheme = "cbcs";
    s.crypt_byte_block = 1;
    s.skip_byte_block = 2;
    auto sub = std::make_unique<ovt::fb::SubsampleRangeT>();
    sub->clear_bytes = 0;
    sub->protected_bytes = 16 * 3 + 5;
    s.subsamples.push_back(std::move(sub));
    CHECK(lengths(iso::sample_clear_protected_ranges(s, 0, 53)) == std::vector<uint64_t>{0, 16, 37});
  }
}

TEST_CASE("cenc: ovt_container_encryption arguments", "[container][cenc][c_api]") {
  MoovSpec m;
  m.samples = {{filled(8, 1), {{5, 59}}, 64}};
  const Opened o(moov_file(m));
  ovt_Buffer b{};
  CHECK(ovt_container_encryption(o.c, 1, 0, 0, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_encryption(o.c, -1, 0, 0, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_encryption(o.c, 0, 0, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_encryption(nullptr, 0, 0, 0, &b) == OVT_E_INVALID_ARG);
}

// ---- the real files (AT1, AT2) --------------------------------------------------------------------------------

namespace {

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string hex_of(const std::vector<uint8_t>& v) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(v.size() * 2);
  for (const uint8_t b : v) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 15]);
  }
  return out;
}

std::unique_ptr<ovt::fb::SampleTableT> sample_table_of(ovt_Container* c) {
  ovt_Buffer b{};
  REQUIRE(ovt_container_sample_table(c, 0, 0, 0, &b) == OVT_OK);
  auto table = std::unique_ptr<ovt::fb::SampleTableT>(flatbuffers::GetRoot<ovt::fb::SampleTable>(b.data)->UnPack());
  delete[] b.data;
  return table;
}

}  // namespace

TEST_CASE("cenc: mpegff_video_cenc_128iv gives the IVs and subsamples of the mp4ff golden",
          "[container][cenc][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("mpegff_video_cenc_128iv");
  std::ifstream golden_in(std::filesystem::path(OVT_TEST_SOURCE_DIR) /
                          "tests/golden/cenc/mpegff_video_cenc_128iv.mp4ff-senc.json");
  REQUIRE(golden_in.good());
  const auto golden = nlohmann::json::parse(golden_in);
  const auto& golden_track = golden.at("tracks").at(0);
  const auto& golden_samples = golden_track.at("samples");
  REQUIRE(golden_samples.size() == 50);

  const Opened o(read_file(path));
  const auto table = sample_table_of(o.c);
  // AT1 asks for samples 0..99; the file has 50, so the range is clamped.
  const auto info = encryption_of(o.c, 0, 100);
  CHECK(info->scheme_type == "cenc");
  CHECK(info->default_iv_size == golden_track.at("per_sample_iv_size").get<int>());
  CHECK(hex_of(info->default_kid) == golden_track.at("default_kid").get<std::string>());
  REQUIRE(info->samples.size() == golden_samples.size());
  REQUIRE(table->size.size() >= golden_samples.size());
  for (size_t i = 0; i < golden_samples.size(); ++i) {
    CAPTURE(i);
    const auto& s = *info->samples[i];
    const auto& g = golden_samples[i];
    CHECK(s.is_protected);
    CHECK(hex_of(s.iv) == g.at("iv").get<std::string>());
    REQUIRE(s.subsamples.size() == g.at("subsamples").size());
    for (size_t k = 0; k < s.subsamples.size(); ++k) {
      CAPTURE(k);
      CHECK(s.subsamples[k]->clear_bytes == g.at("subsamples")[k].at("clear").get<uint32_t>());
      CHECK(s.subsamples[k]->protected_bytes == g.at("subsamples")[k].at("protected").get<uint32_t>());
    }
    CHECK(sum_of(s) == table->size[i]);  // Σ(clear + protected) == the sample size
  }
  for (const auto& d : info->diagnostics) {
    INFO(d->code + ": " + d->message);
    CHECK(d->severity != ovt::fb::Severity::Error);
  }
}

TEST_CASE("cenc: syn_mp4_cenc_cbcs uses the constant IV and shades 16 of every 160 bytes",
          "[container][cenc][needs-corpus]") {
  // The entry's files sit side by side; the primary one is syn_mp4_elst.mp4 (recipe 20 (a)).
  const auto primary = ovt::test::corpus_or_skip("syn_mp4_edit_list_frag_cenc");
  const auto path = primary.parent_path() / "syn_mp4_cenc_cbcs.mp4";
  if (!std::filesystem::is_regular_file(path)) {
    INFO("corpus file missing: syn_mp4_cenc_cbcs.mp4");
    CHECK_FALSE(ovt::test::require_corpora());
    SKIP("syn_mp4_cenc_cbcs.mp4 is not in the corpus cache");
  }
  const Opened o(read_file(path));
  const auto table = sample_table_of(o.c);
  const auto info = encryption_of(o.c);
  CHECK(info->scheme_type == "cbcs");
  CHECK(info->pattern_crypt == 1);
  CHECK(info->pattern_skip == 9);
  REQUIRE(info->default_constant_iv.size() == 16);
  REQUIRE(info->samples.size() == table->count);
  size_t pattern_gaps = 0;
  for (size_t i = 0; i < info->samples.size(); ++i) {
    CAPTURE(i);
    const auto& s = *info->samples[i];
    REQUIRE(s.is_protected);
    CHECK(s.iv == info->default_constant_iv);  // AT2: iv == tenc.default_constant_IV
    CHECK(sum_of(s) == table->size[i]);
    const auto ranges = iso::sample_clear_protected_ranges(s, table->offset[i], table->size[i]);
    uint64_t at = table->offset[i];
    for (size_t r = 0; r < ranges.size(); ++r) {
      CHECK(ranges[r].offset == at);  // the ranges tile the sample
      at += ranges[r].length;
      if (r % 2 == 1) {
        CHECK(ranges[r].length == 16);  // 16 encrypted bytes (a partial trailing block is clear for cbcs)
      } else if (r >= 2 && r + 1 < ranges.size() && ranges[r].length == 144) {
        ++pattern_gaps;  // 144 clear bytes between two crypt blocks of one protected span
      }
    }
    CHECK(at == table->offset[i] + table->size[i]);
  }
  CHECK(pattern_gaps > 0);
}
