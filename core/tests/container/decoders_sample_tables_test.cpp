// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group D part 1 hand decoders (chapter 08 §8.3.4, WI-1.209): `stts`, `ctts`, `cslg`, `stsc`, `stsz`,
// `stz2`, `stco`, `co64`, `stss`, `stps`, `sdtp`, the CNT-20xx box-level checks and the windowed reads of tables larger
// than 1 MiB (PagedSpan, issue #630).

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/paged_span.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt_container_generated.h"

namespace {

namespace iso = ovt::container::isobmff;
constexpr uint64_t kMiB = uint64_t{1} << 20U;
constexpr uint64_t kPage = uint64_t{64} << 10U;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;
using ovt::fb::FieldT;

void put(Bytes& b, uint64_t v, unsigned nbytes) {
  for (unsigned i = nbytes; i > 0; --i) {
    b.push_back(static_cast<uint8_t>(v >> (8U * (i - 1U))));
  }
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes u32s(std::initializer_list<uint32_t> values) {
  Bytes b;
  for (const uint32_t v : values) {
    put(b, v, 4);
  }
  return b;
}
Bytes box(std::string_view type, const Bytes& payload) {
  Bytes b;
  put(b, 8 + payload.size(), 4);
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
Bytes full(std::string_view type, uint8_t version, const Bytes& payload) {
  return box(type, cat({Bytes{version, 0, 0, 0}, payload}));
}

// ftyp, moov/trak/mdia/minf/stbl(children) and a small mdat.
Bytes file_with(const std::vector<Bytes>& stbl_children) {
  Bytes children;
  for (const Bytes& c : stbl_children) {
    children.insert(children.end(), c.begin(), c.end());
  }
  const Bytes stbl = box("stbl", children);
  return cat({box("ftyp", cat({Bytes{'i', 's', 'o', 'm'}, u32s({0})})),
              box("moov", box("trak", box("mdia", box("minf", stbl)))), box("mdat", Bytes(64, 0))});
}

struct Walked {
  iso::WalkResult result;
  const BoxNodeT* stbl = nullptr;
};

const BoxNodeT* child(const BoxNodeT& n, std::string_view type) {
  const auto it = std::ranges::find_if(n.children, [&](const auto& c) { return c->fourcc == type; });
  return it == n.children.end() ? nullptr : it->get();
}

Walked walk_stbl(const Bytes& data) {
  const ovt::io::MemoryFile f(data);
  Walked w;
  w.result = iso::walk(f);
  const BoxNodeT* n = nullptr;
  for (const auto& top : w.result.top_level) {
    if (top->fourcc == "moov") {
      n = top.get();
    }
  }
  for (const std::string_view t : {"trak", "mdia", "minf", "stbl"}) {
    REQUIRE(n != nullptr);
    n = child(*n, t);
  }
  w.stbl = n;
  REQUIRE(w.stbl != nullptr);
  return w;
}

const BoxNodeT& box_of(const Walked& w, std::string_view type) {
  const BoxNodeT* n = child(*w.stbl, type);
  REQUIRE(n != nullptr);
  return *n;
}

const FieldT* field(const std::vector<std::unique_ptr<FieldT>>& fields, std::string_view name) {
  const auto it = std::ranges::find_if(fields, [&](const auto& f) { return f->name == name; });
  return it == fields.end() ? nullptr : it->get();
}

const FieldT& entry(const BoxNodeT& n, uint32_t i) {
  const FieldT* entries = field(n.fields, "entries");
  REQUIRE(entries != nullptr);
  const FieldT* e = field(entries->children, "entry[" + std::to_string(i) + "]");
  REQUIRE(e != nullptr);
  return *e;
}

int64_t ivalue_of(const FieldT& f, std::string_view name) {
  const FieldT* c = field(f.children, name);
  REQUIRE(c != nullptr);
  return c->kind == ovt::fb::SyntaxValueKind::Int ? c->ivalue : static_cast<int64_t>(c->uvalue);
}

const ovt::fb::DiagnosticT* diag_of(const BoxNodeT& n, std::string_view code) {
  const auto it = std::ranges::find_if(n.diagnostics, [&](const auto& d) { return d->code == code; });
  return it == n.diagnostics.end() ? nullptr : it->get();
}

bool has(const BoxNodeT& n, std::string_view code) {
  return diag_of(n, code) != nullptr;
}

Bytes read_broken(const char* name) {
  const auto path = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/synthetic/isobmff-broken" / name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every top-level field starts where the previous one ends, and the last one ends with the payload.
void check_contiguous(const BoxNodeT& n) {
  uint64_t at = uint64_t{n.header_length} * 8U;
  for (const auto& f : n.fields) {
    INFO(f->name);
    CHECK(f->span->bit_offset() == at);
    at += f->span->bit_length();
  }
  CHECK(at == n.file_span->length() * 8U);
}

}  // namespace

TEST_CASE("stts: a zero delta before the last sample raises CNT-2001", "[container][decoders][stbl][stts]") {
  const Bytes data = file_with({full("stts", 0, u32s({3, 100, 1024, 5, 0, 7, 0}))});
  const Walked stts_walked = walk_stbl(data);
  const BoxNodeT& stts = box_of(stts_walked, "stts");
  CHECK(diag_of(stts, "CNT-2001")->message.starts_with("2 entries with sample_delta 0"));
  CHECK(ivalue_of(entry(stts, 1), "sample_count") == 5);
  CHECK(ivalue_of(entry(stts, 1), "sample_delta") == 0);
}

TEST_CASE("stts: a clean table has run statistics and no findings", "[container][decoders][stbl][stts]") {
  const Bytes data = file_with({full("stts", 0, u32s({2, 100, 1000, 1, 0}))});  // a last sample may have delta 0
  const Walked w = walk_stbl(data);
  const BoxNodeT& stts = box_of(w, "stts");
  CHECK(stts.diagnostics.empty());
  CHECK(field(stts.fields, "entries")->uvalue == 2);
  CHECK(field(stts.fields, "entries")->svalue.starts_with("101 samples, duration 100000.000 ticks"));
  check_contiguous(stts);
}

TEST_CASE("ctts v0 offsets of 2^31 or more read as negative with CNT-2002", "[container][decoders][stbl][ctts]") {
  const Bytes data = file_with({full("ctts", 0, u32s({2, 1, 5, 1, 0x80000000U}))});
  const Walked w = walk_stbl(data);
  const BoxNodeT& ctts = box_of(w, "ctts");
  CHECK(has(ctts, "CNT-2002"));
  CHECK(ivalue_of(entry(ctts, 1), "sample_offset") < 0);
  CHECK(field(ctts.fields, "entries")->svalue == "2 samples, offsets -2147483648 to 5 ticks");
  check_contiguous(ctts);
}

TEST_CASE("ctts v1 keeps negative offsets without a finding", "[container][decoders][stbl][ctts]") {
  const Bytes data = file_with({full("ctts", 1, u32s({1, 4, 0xFFFFFFFEU}))});
  const Walked ctts_walked = walk_stbl(data);
  const BoxNodeT& ctts = box_of(ctts_walked, "ctts");
  CHECK_FALSE(has(ctts, "CNT-2002"));
  CHECK(ivalue_of(entry(ctts, 0), "sample_offset") == -2);
}

TEST_CASE("ctts_v0_negative.mp4 raises CNT-2002 and the offset is negative", "[container][decoders][stbl][ctts]") {
  const Bytes data = read_broken("ctts_v0_negative.mp4");
  const ovt::io::MemoryFile f(data);
  const iso::WalkResult r = iso::walk(f);
  bool found = false;
  const auto visit = [&](const auto& self, const BoxNodeT& n) -> void {
    if (n.fourcc == "ctts") {
      found = true;
      CHECK(has(n, "CNT-2002"));
      CHECK(diag_of(n, "CNT-2002")->severity == ovt::fb::Severity::Warning);
      CHECK(ivalue_of(entry(n, 0), "sample_offset") < 0);
    }
    for (const auto& c : n.children) {
      self(self, *c);
    }
  };
  for (const auto& top : r.top_level) {
    visit(visit, *top);
  }
  CHECK(found);
}

TEST_CASE("cslg v0 and v1 with seconds", "[container][decoders][stbl][cslg]") {
  Bytes v1;
  for (const int64_t v : {int64_t{0}, int64_t{-2}, int64_t{3}, int64_t{0}, int64_t{100}}) {
    put(v1, static_cast<uint64_t>(v), 8);
  }
  const Bytes data = file_with({full("cslg", 0, u32s({1, 0xFFFFFFFEU, 3, 0, 100})), full("cslg", 1, v1)});
  const Walked w = walk_stbl(data);
  const BoxNodeT& v0box = *w.stbl->children[0];
  CHECK(field(v0box.fields, "leastDecodeToDisplayDelta")->ivalue == -2);
  CHECK(field(v0box.fields, "compositionEndTime")->ivalue == 100);
  check_contiguous(v0box);
  const BoxNodeT& v1box = *w.stbl->children[1];
  CHECK(field(v1box.fields, "leastDecodeToDisplayDelta")->ivalue == -2);
  CHECK(field(v1box.fields, "compositionToDTSShift")->kind == ovt::fb::SyntaxValueKind::Int);
  check_contiguous(v1box);
}

TEST_CASE("stsc checks first_chunk order and sample_description_index", "[container][decoders][stbl][stsc]") {
  const Bytes good = file_with({full("stsd", 0, u32s({1})), full("stsc", 0, u32s({2, 1, 15, 1, 4, 6, 1}))});
  CHECK(box_of(walk_stbl(good), "stsc").diagnostics.empty());
  const Bytes not_one = file_with({full("stsc", 0, u32s({1, 2, 3, 1}))});
  CHECK(has(box_of(walk_stbl(not_one), "stsc"), "CNT-2003"));
  const Bytes descending = file_with({full("stsc", 0, u32s({3, 1, 3, 1, 5, 3, 1, 4, 3, 1}))});
  CHECK(has(box_of(walk_stbl(descending), "stsc"), "CNT-2003"));
  const Bytes bad_index =
      file_with({full("stsd", 0, u32s({1})), full("stsc", 0, u32s({3, 1, 1, 0, 2, 1, 2, 3, 1, 1}))});
  const Walked stsc_walked = walk_stbl(bad_index);
  const BoxNodeT& stsc = box_of(stsc_walked, "stsc");
  CHECK(has(stsc, "CNT-2004"));
  CHECK(diag_of(stsc, "CNT-2004")->message.starts_with("2 entries"));
}

TEST_CASE("stsc_descending.mp4 raises CNT-2003 as an Error", "[container][decoders][stbl][stsc]") {
  const Bytes data = read_broken("stsc_descending.mp4");
  const ovt::io::MemoryFile f(data);
  const iso::WalkResult r = iso::walk(f);
  bool found = false;
  const auto visit = [&](const auto& self, const BoxNodeT& n) -> void {
    if (const auto* d = diag_of(n, "CNT-2003"); d != nullptr && n.fourcc == "stsc") {
      found = d->severity == ovt::fb::Severity::Error;
    }
    for (const auto& c : n.children) {
      self(self, *c);
    }
  };
  for (const auto& top : r.top_level) {
    visit(visit, *top);
  }
  CHECK(found);
}

TEST_CASE("stsz: per-sample sizes, a constant size and zero-size samples", "[container][decoders][stbl][stsz]") {
  const Walked w = walk_stbl(file_with({full("stsz", 0, u32s({0, 4, 10, 0, 30, 40}))}));
  const BoxNodeT& stsz = box_of(w, "stsz");
  CHECK(field(stsz.fields, "sample_count")->uvalue == 4);
  CHECK(field(stsz.fields, "entries")->svalue == "total 80 bytes, min 0, max 40, mean 20.000");
  CHECK(has(stsz, "CNT-2013"));
  CHECK(ivalue_of(entry(stsz, 3), "entry_size") == 40);
  check_contiguous(stsz);
  const Walked constant_walked = walk_stbl(file_with({full("stsz", 0, u32s({512, 100}))}));
  const BoxNodeT& constant = box_of(constant_walked, "stsz");
  CHECK(field(constant.fields, "sample_count")->svalue == "total 51200 bytes");
  CHECK(field(constant.fields, "entries") == nullptr);
}

TEST_CASE("stsz with a sample_count the payload cannot hold raises CNT-1014", "[container][decoders][stbl][stsz]") {
  const Walked stsz_walked = walk_stbl(file_with({full("stsz", 0, u32s({0, 1000, 5, 6}))}));
  const BoxNodeT& stsz = box_of(stsz_walked, "stsz");
  CHECK(has(stsz, "CNT-1014"));
  CHECK(field(stsz.fields, "entries")->uvalue == 2);  // the entries present; sample_count keeps the declared number
  CHECK(field(stsz.fields, "sample_count")->uvalue == 1000);
}

TEST_CASE("stz2 unpacks 4, 8 and 16 bit entries, high nibble first", "[container][decoders][stbl][stz2]") {
  const Bytes four = full("stz2", 0, cat({Bytes{0, 0, 0, 4}, u32s({5}), Bytes{0x12, 0x34, 0x50}}));
  const Walked stz2_walked = walk_stbl(file_with({four}));
  const BoxNodeT& stz2 = box_of(stz2_walked, "stz2");
  for (uint32_t i = 0; i < 5; ++i) {
    CHECK(ivalue_of(entry(stz2, i), "entry_size") == i + 1);
  }
  CHECK(field(stz2.fields, "entries")->svalue == "total 15 bytes, min 1, max 5, mean 3.000");
  check_contiguous(stz2);
  const Bytes sixteen = full("stz2", 0, cat({Bytes{0, 0, 0, 16}, u32s({2}), Bytes{0x01, 0x00, 0xFF, 0xFF}}));
  const Walked wide_walked = walk_stbl(file_with({sixteen}));
  const BoxNodeT& wide = box_of(wide_walked, "stz2");
  CHECK(ivalue_of(entry(wide, 0), "entry_size") == 256);
  CHECK(ivalue_of(entry(wide, 1), "entry_size") == 65535);
  const Bytes bad = full("stz2", 0, cat({Bytes{0, 0, 0, 12}, u32s({1}), Bytes{1, 2}}));
  const Walked stz2_bad_walked = walk_stbl(file_with({bad}));
  const BoxNodeT& stz2_bad = box_of(stz2_bad_walked, "stz2");
  CHECK(has(stz2_bad, "CNT-2006"));
  CHECK(field(stz2_bad.fields, "entries") == nullptr);
}

TEST_CASE("stco and co64: offsets, statistics and CNT-2007", "[container][decoders][stbl][stco]") {
  const Bytes data = file_with({full("stco", 0, u32s({3, 100, 90000000, 200}))});
  const Walked stco_walked = walk_stbl(data);
  const BoxNodeT& stco = box_of(stco_walked, "stco");
  CHECK(has(stco, "CNT-2007"));  // 90 000 000 is past the end of this small file
  CHECK(field(stco.fields, "entries")->svalue == "3 chunks, offsets 100 to 90000000");
  CHECK(ivalue_of(entry(stco, 1), "chunk_offset") == 90000000);
  Bytes co;
  put(co, 1, 4);
  put(co, uint64_t{1} << 40U, 8);
  const Walked co64_walked = walk_stbl(file_with({full("co64", 0, co)}));
  const BoxNodeT& co64 = box_of(co64_walked, "co64");
  CHECK(ivalue_of(entry(co64, 0), "chunk_offset") == (int64_t{1} << 40));
  CHECK(has(co64, "CNT-2007"));
  check_contiguous(co64);
}

TEST_CASE("stco_beyond_eof.mp4 raises CNT-2007", "[container][decoders][stbl][stco]") {
  const Bytes data = read_broken("stco_beyond_eof.mp4");
  const ovt::io::MemoryFile f(data);
  const iso::WalkResult r = iso::walk(f);
  bool found = false;
  const auto visit = [&](const auto& self, const BoxNodeT& n) -> void {
    found = found || (n.fourcc == "stco" && has(n, "CNT-2007"));
    for (const auto& c : n.children) {
      self(self, *c);
    }
  };
  for (const auto& top : r.top_level) {
    visit(visit, *top);
  }
  CHECK(found);
}

TEST_CASE("stss and stps: order, statistics and the range against the sample count",
          "[container][decoders][stbl][stss]") {
  const Walked w = walk_stbl(file_with(
      {full("stss", 0, u32s({3, 1, 31, 61})), full("stps", 0, u32s({2, 8, 8})), full("stsz", 0, u32s({4, 100}))}));
  const BoxNodeT& stss = box_of(w, "stss");
  CHECK(field(stss.fields, "entries")->svalue == "3 sync samples, mean GOP length 30.000");
  CHECK(stss.diagnostics.empty());
  CHECK(has(box_of(w, "stps"), "CNT-2009"));  // 8, 8: not strictly increasing
  const Walked past = walk_stbl(file_with({full("stss", 0, u32s({2, 1, 61})), full("stsz", 0, u32s({4, 50}))}));
  CHECK(has(box_of(past, "stss"), "CNT-2009"));  // 61 > 50 samples, found once stsz is known
  const Walked all = walk_stbl(file_with({full("stsz", 0, u32s({4, 2})), full("stss", 0, u32s({2, 1, 2}))}));
  CHECK(has(box_of(all, "stss"), "CNT-2019"));
}

TEST_CASE("sdtp decodes the four 2-bit fields and its length is checked in either box order",
          "[container][decoders][stbl][sdtp]") {
  const Bytes sdtp = full("sdtp", 0, Bytes{0x24, 0xE4});  // 00 10 01 00, 11 10 01 00
  const Bytes stsz = full("stsz", 0, u32s({7, 2}));
  const Walked ok_walked = walk_stbl(file_with({stsz, sdtp}));
  const BoxNodeT& ok = box_of(ok_walked, "sdtp");
  CHECK(ok.diagnostics.empty());
  const FieldT& e0 = entry(ok, 0);
  CHECK(field(e0.children, "sample_depends_on")->svalue == "does not depend on others (I picture)");
  CHECK(field(e0.children, "is_leading")->uvalue == 0);
  CHECK(field(entry(ok, 1).children, "is_leading")->uvalue == 3);
  check_contiguous(ok);
  // sdtp before stsz: the length is checked once the children of stbl are known.
  CHECK_FALSE(has(box_of(walk_stbl(file_with({sdtp, stsz})), "sdtp"), "CNT-2010"));
  const Bytes stsz3 = full("stsz", 0, u32s({7, 3}));
  CHECK(has(box_of(walk_stbl(file_with({sdtp, stsz3})), "sdtp"), "CNT-2010"));
  CHECK(has(box_of(walk_stbl(file_with({stsz3, sdtp})), "sdtp"), "CNT-2010"));
}

TEST_CASE("sample count checks between stts, ctts, stsz and stco/co64", "[container][decoders][stbl][cross]") {
  const Bytes stts = full("stts", 0, u32s({1, 10, 1000}));
  const Bytes ctts = full("ctts", 0, u32s({1, 9, 0}));
  const Bytes stsz = full("stsz", 0, u32s({100, 12}));
  const Walked w = walk_stbl(file_with({stts, ctts, stsz}));
  CHECK(has(box_of(w, "stsz"), "CNT-2005"));
  CHECK(diag_of(box_of(w, "stsz"), "CNT-2005")->message == "stts covers 10 samples, stsz 12");
  CHECK(has(box_of(w, "ctts"), "CNT-2011"));
  CHECK_FALSE(
      has(box_of(walk_stbl(file_with({stsz, stts})), "stts"), "CNT-2005"));  // reported on stsz, whatever the order
  CHECK(has(box_of(walk_stbl(file_with({stsz, stts})), "stsz"), "CNT-2005"));
  const Bytes both = file_with({full("stco", 0, u32s({0})), full("co64", 0, u32s({0}))});
  CHECK(has(box_of(walk_stbl(both), "co64"), "CNT-2008"));
}

TEST_CASE("every decoder fails soft on truncated and hostile payloads", "[container][decoders][stbl]") {
  for (const std::string_view type :
       {"stts", "ctts", "cslg", "stsc", "stsz", "stz2", "stco", "co64", "stss", "stps", "sdtp"}) {
    INFO(type);
    for (const Bytes& payload : {Bytes{}, Bytes{0, 0}, u32s({0xFFFFFFFFU}), u32s({0xFFFFFFFFU, 0xFFFFFFFFU, 1})}) {
      const Walked w = walk_stbl(file_with({full(type, 0, payload), full(type, 1, payload)}));
      CHECK(child(*w.stbl, type) != nullptr);
    }
  }
}

namespace {

// A RandomAccessFile over memory that counts the bytes it serves.
class CountingFile final : public ovt::io::RandomAccessFile {
 public:
  explicit CountingFile(Bytes data)
      : RandomAccessFile(ovt::io::FileInfo{data.size(), 0, "counting"}), data_(std::move(data)) {}
  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset >= data_.size()) {
      return 0;
    }
    const size_t n = std::min<size_t>(len, data_.size() - offset);
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), n, dst);
    served_ += n;
    return n;
  }
  [[nodiscard]] Holder view(uint64_t /*offset*/, size_t /*len*/) const override { return {}; }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*a*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }
  [[nodiscard]] uint64_t served() const noexcept { return served_; }

 private:
  Bytes data_;
  mutable std::atomic<uint64_t> served_{0};
};

// Claims `claimed` bytes but holds only the first `data.size()`: a file that shrank under the reader.
class ShrunkFile final : public ovt::io::RandomAccessFile {
 public:
  ShrunkFile(Bytes data, uint64_t claimed)
      : RandomAccessFile(ovt::io::FileInfo{claimed, 0, "shrunk"}), data_(std::move(data)) {}
  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset >= data_.size()) {
      return 0;
    }
    const size_t n = std::min<size_t>(len, data_.size() - offset);
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), n, dst);
    return n;
  }
  [[nodiscard]] Holder view(uint64_t /*offset*/, size_t /*len*/) const override { return {}; }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*a*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }

 private:
  Bytes data_;
};

Bytes stsz_table(uint32_t count) {
  Bytes entries;
  for (uint32_t i = 0; i < count; ++i) {
    put(entries, 1000 + i % 7, 4);
  }
  return full("stsz", 0, cat({u32s({0, count}), entries}));
}

}  // namespace

TEST_CASE("PagedSpan serves windows of at most 1 MiB in whole 64 KiB pages", "[container][isobmff][paged_span]") {
  Bytes data(5 * kMiB + 123);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i * 31U);
  }
  const CountingFile file(data);
  iso::PagedSpan span(file, 1000, data.size() - 1000);
  CHECK(span.size() == data.size() - 1000);
  const auto w = span.window(70000, 100);
  REQUIRE(w.size() == 100);
  CHECK(w[0] == data[1000 + 70000]);
  CHECK(span.bytes_fetched() == kPage);  // the one page that holds the window (page 1)
  CHECK(span.window(70010, 50).size() == 50);
  CHECK(span.bytes_fetched() == kPage);  // served from the page already held
  CHECK(span.window(0, 8U << 20U).size() == iso::PagedSpan::kMaxWindow);
  const auto tail = span.window(span.size() - 10, 100);  // clamped to the end
  CHECK(tail.size() == 10);
  CHECK(tail[9] == data.back());
  CHECK(span.window(span.size(), 4).empty());
  const iso::PagedSpan mem(std::span<const uint8_t>(data).subspan(10, 20));
  CHECK(mem.size() == 20);
}

TEST_CASE("a table larger than 1 MiB is decoded in windows with exact spans and statistics",
          "[container][decoders][stbl][windowed]") {
  constexpr uint32_t kCount = 600000;  // 2.4 MB of entries
  const Bytes data = file_with({stsz_table(kCount)});
  auto file = std::make_unique<CountingFile>(data);
  const CountingFile* raw = file.get();
  const iso::WalkResult r = iso::walk(*file);
  const BoxNodeT* n = r.top_level[1]->children[0]->children[0]->children[0]->children[0].get();
  const BoxNodeT& stsz = *child(*n, "stsz");
  const FieldT* entries = field(stsz.fields, "entries");
  REQUIRE(entries != nullptr);
  CHECK(entries->uvalue == kCount);
  CHECK(entries->children.size() == 80);  // 64 first and 16 last (§8.2.5)
  // entry i starts 8 (header) + 4 (version/flags) + 8 (sample_size, sample_count) bytes after the box start.
  const uint64_t base_bits = uint64_t{8 + 4 + 8} * 8U;
  CHECK(entries->span->bit_offset() == base_bits);
  CHECK(entries->span->bit_length() == uint64_t{kCount} * 32U);
  const FieldT& last = entry(stsz, kCount - 1);
  CHECK(last.span->bit_offset() == base_bits + uint64_t{kCount - 1} * 32U);
  CHECK(ivalue_of(last, "entry_size") == 1000 + (kCount - 1) % 7);
  CHECK(field(entries->children, "entry[64]") == nullptr);
  CHECK(field(entries->children, "entry[" + std::to_string(kCount - 17) + "]") == nullptr);
  CHECK(field(entries->children, "entry[" + std::to_string(kCount - 16) + "]") != nullptr);
  uint64_t total = 0;
  for (uint32_t i = 0; i < kCount; ++i) {
    total += 1000 + i % 7;
  }
  CHECK(entries->svalue.starts_with("total " + std::to_string(total) + " bytes, min 1000, max 1006, mean "));
  // The table was read once, in 64 KiB pages: no more than the file plus two pages of slack per window.
  CHECK(raw->served() < data.size() + 3 * kMiB);
}

TEST_CASE("108 000 samples serialise to at most 8 MiB with 80 materialised entries",
          "[container][decoders][stbl][stsz]") {
  // WI-1.209 acceptance test 1 at the size of recipe 27 (syn_h264_1h_1080p: 108 000 frames).
  const Bytes data = file_with({stsz_table(108000)});
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(data)};
  ovt_Container* container = nullptr;
  REQUIRE(ovt_container_open(&handle, &container) == OVT_OK);
  const ovt_Buffer buf = ovt_container_tree(container);
  CHECK(buf.size <= 8 * kMiB);
  flatbuffers::Verifier v(buf.data, buf.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::ContainerTree>(nullptr));
  const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data);
  const ovt::fb::BoxNode* n = tree->root()->Get(1);
  for (int level = 0; level < 4; ++level) {
    n = n->children()->Get(0);
  }
  const ovt::fb::BoxNode* stsz = n->children()->Get(0);
  REQUIRE(stsz->fourcc()->string_view() == "stsz");
  for (const auto* f : *stsz->fields()) {
    if (f->name()->string_view() == "sample_count") {
      CHECK(f->uvalue() == 108000);
    }
    if (f->name()->string_view() == "entries") {
      CHECK(f->uvalue() == 108000);
      CHECK(f->children()->size() == 80);
    }
  }
  ovt_container_close(container);
}

TEST_CASE("a window that comes back short gives CNT-1002 at the window start",
          "[container][decoders][stbl][windowed]") {
  constexpr uint32_t kCount = 400000;  // 1.6 MB: the second 1 MiB window starts at entry 262144
  const Bytes data = file_with({stsz_table(kCount)});
  Bytes cut(data.begin(), data.begin() + 1'200'000);
  const ShrunkFile file(std::move(cut), data.size());
  const iso::WalkResult r = iso::walk(file);
  const BoxNodeT* n = r.top_level[1]->children[0]->children[0]->children[0]->children[0].get();
  const BoxNodeT& stsz = *child(*n, "stsz");
  const auto* d = diag_of(stsz, "CNT-1002");
  REQUIRE(d != nullptr);
  const uint64_t base_bits = uint64_t{8 + 4 + 8} * 8U;
  CHECK(d->span->bit_offset() == base_bits + uint64_t{262144} * 32U);
  CHECK(field(stsz.fields, "entries") != nullptr);
}

TEST_CASE("a constant-size stsz counts its trailing bytes from the whole payload",
          "[container][decoders][stbl][stsz]") {
  const Bytes data = file_with({full("stsz", 0, cat({u32s({512, 100}), Bytes(7, 0)}))});
  const Walked w = walk_stbl(data);
  CHECK(diag_of(box_of(w, "stsz"), "CNT-1005")->message.starts_with("7 unparsed"));
}
