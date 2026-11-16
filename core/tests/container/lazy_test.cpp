// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the lazy ISOBMFF traversal (chapter 08 §8.2.4, WI-1.204): header-only `moof`s, ovt_container_expand,
// the `mfro`/`mfra` location, and the bytes read when opening files with a huge `mdat`.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/lazy.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt_container_generated.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace iso = ovt::container::isobmff;

constexpr uint64_t kGiB = 1ULL << 30U;
constexpr uint64_t kMiB = 1ULL << 20U;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
void put64(Bytes& b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v >> 32U));
  put32(b, static_cast<uint32_t>(v));
}
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
void put_all(Bytes& b, const Bytes& more) {
  b.insert(b.end(), more.begin(), more.end());
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  put_str(b, type);
  put_all(b, payload);
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    put_all(b, p);
  }
  return b;
}
Bytes full_box_payload(uint32_t value) {  // version/flags 0 and one u32
  Bytes b(4, 0);
  put32(b, value);
  return b;
}
// A box whose header claims `total` bytes through largesize; the caller supplies the bytes that really exist.
Bytes large_box_header(std::string_view type, uint64_t total) {
  Bytes b;
  put32(b, 1);
  put_str(b, type);
  put64(b, total);
  return b;
}

// Chapter 8 §8.2.2 (issue #630): RandomAccessFile has no bytes_read(); this subclass counts the bytes served.
// A sparse file: the listed pieces are real, every other byte below `size` reads as zero. Counts the bytes served.
class SparseFile final : public ovt::io::RandomAccessFile {
 public:
  SparseFile(uint64_t size, std::map<uint64_t, Bytes> pieces)
      : RandomAccessFile(ovt::io::FileInfo{size, 0, "sparse"}), pieces_(std::move(pieces)) {}

  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset >= size()) {
      return 0;
    }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len, size() - offset));
    std::fill_n(dst, n, uint8_t{0});
    for (const auto& [at, bytes] : pieces_) {
      const uint64_t lo = std::max<uint64_t>(at, offset);
      const uint64_t hi = std::min<uint64_t>(at + bytes.size(), offset + n);
      if (lo < hi) {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(lo - at), hi - lo, dst + (lo - offset));
      }
    }
    bytes_read_ += n;
    return n;
  }
  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override {
    auto buf = std::make_shared<Bytes>(len);
    buf->resize(read(offset, len, buf->data()));
    return Holder{buf, std::span<const uint8_t>(*buf)};
  }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*a*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }
  [[nodiscard]] uint64_t bytes_read() const noexcept { return bytes_read_; }

 private:
  std::map<uint64_t, Bytes> pieces_;
  mutable std::atomic<uint64_t> bytes_read_{0};
};

const ovt::fb::ContainerTree* tree_of(ovt_Container* c) {
  const ovt_Buffer b = ovt_container_tree(c);
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::ContainerTree>(nullptr));
  return flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data);
}

const ovt::fb::BoxNode* child(const ovt::fb::BoxNode* n, std::string_view type) {
  if (n->children() != nullptr) {
    for (const auto* c : *n->children()) {
      if (c->fourcc()->string_view() == type) {
        return c;
      }
    }
  }
  return nullptr;
}

bool has_diag(const ovt::fb::BoxNode* n, std::string_view code) {
  return n->diagnostics() != nullptr &&
         std::ranges::any_of(*n->diagnostics(), [&](const auto* d) { return d->code()->string_view() == code; });
}

// moof(mfhd(sequence), traf(tfhd, tfdt, trun)) followed by an mdat of `mdat_bytes`.
Bytes fragment(uint32_t sequence, size_t mdat_bytes = 16) {
  return cat({box("moof", cat({box("mfhd", full_box_payload(sequence)),
                               box("traf", cat({box("tfhd", full_box_payload(1)), box("tfdt", full_box_payload(0)),
                                                box("trun", full_box_payload(0))}))})),
              box("mdat", Bytes(mdat_bytes, 0xAA))});
}

Bytes fragmented_file(unsigned fragments) {
  Bytes f = cat({box("ftyp", cat({Bytes{'i', 's', 'o', '6'}, full_box_payload(0)})),
                 box("moov", cat({box("mvhd", Bytes(100, 0)), box("mvex", box("trex", Bytes(24, 0)))}))});
  for (unsigned i = 1; i <= fragments; ++i) {
    put_all(f, fragment(i));
  }
  return f;
}

std::vector<uint64_t> moof_offsets(const ovt::fb::ContainerTree* t) {
  std::vector<uint64_t> out;
  for (const auto* n : *t->root()) {
    if (n->fourcc()->string_view() == "moof") {
      out.push_back(n->file_span()->offset());
    }
  }
  return out;
}

struct Opened {
  ovt_File handle;
  ovt_Container* container = nullptr;
  explicit Opened(std::unique_ptr<ovt::io::RandomAccessFile> f) : handle{std::move(f)} {
    REQUIRE(ovt_container_open(&handle, &container) == OVT_OK);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  ~Opened() { ovt_container_close(container); }
};

}  // namespace

TEST_CASE("lazy: moov first with a multi-GiB mdat reads only headers", "[container][isobmff][lazy][fast]") {
  // ftyp, a moov with a large `stsz`-sized leaf, then an mdat that claims 8 GiB through largesize.
  const Bytes ftyp = box("ftyp", cat({Bytes{'i', 's', 'o', 'm'}, full_box_payload(0)}));
  const Bytes stsz = box("stsz", Bytes(4 * kMiB, 0));
  const Bytes moov =
      box("moov", cat({box("mvhd", Bytes(100, 0)), box("trak", box("mdia", box("minf", box("stbl", stsz))))}));
  const uint64_t mdat_at = ftyp.size() + moov.size();
  auto file = std::make_unique<SparseFile>(
      mdat_at + 8 * kGiB,
      std::map<uint64_t, Bytes>{{0, cat({ftyp, moov})}, {mdat_at, large_box_header("mdat", 8 * kGiB)}});
  const SparseFile* raw = file.get();
  Opened o(std::move(file));
  CHECK(raw->bytes_read() < 16 * kMiB);
  CHECK(raw->bytes_read() < kMiB);  // nothing here decodes `stsz`, so its payload is never touched either
  const auto* t = tree_of(o.container);
  REQUIRE(t->root()->size() == 3);
  const auto* mdat = t->root()->Get(2);
  CHECK(mdat->fourcc()->string_view() == "mdat");
  CHECK(mdat->file_span()->length() == 8 * kGiB);
  CHECK(mdat->children() == nullptr);
  CHECK_FALSE(has_diag(t->root()->Get(1), "CNT-0003"));
}

TEST_CASE("lazy: moov after a 2 GiB mdat emits CNT-0003 and keeps the moov tree", "[container][isobmff][lazy][fast]") {
  const Bytes ftyp = box("ftyp", cat({Bytes{'i', 's', 'o', 'm'}, full_box_payload(0)}));
  const Bytes moov = box("moov", cat({box("mvhd", Bytes(100, 0)), box("trak", box("tkhd", Bytes(84, 0)))}));
  const uint64_t mdat_at = ftyp.size();
  const uint64_t moov_at = mdat_at + 2 * kGiB;
  auto file = std::make_unique<SparseFile>(
      moov_at + moov.size(),
      std::map<uint64_t, Bytes>{{0, ftyp}, {mdat_at, large_box_header("mdat", 2 * kGiB)}, {moov_at, moov}});
  const SparseFile* raw = file.get();
  Opened o(std::move(file));
  CHECK(raw->bytes_read() < 16 * kMiB);
  const auto* t = tree_of(o.container);
  REQUIRE(t->root()->size() == 3);
  const auto* m = t->root()->Get(2);
  REQUIRE(m->fourcc()->string_view() == "moov");
  CHECK(has_diag(m, "CNT-0003"));
  CHECK(child(m, "trak") != nullptr);
  CHECK(child(m, "mvhd") != nullptr);
}

TEST_CASE("lazy: 3600 moofs are header-only until expanded", "[container][isobmff][lazy][fast]") {
  const Bytes data = fragmented_file(3600);
  Opened o(std::make_unique<ovt::io::MemoryFile>(data));
  const auto offsets = [&] { return moof_offsets(tree_of(o.container)); }();
  REQUIRE(offsets.size() == 3600);
  {
    const auto* t = tree_of(o.container);
    for (const auto* n : *t->root()) {
      if (n->fourcc()->string_view() != "moof") {
        continue;
      }
      REQUIRE(n->children()->size() == 1);
      CHECK(child(n, "mfhd") != nullptr);
      CHECK(child(n, "traf") == nullptr);
      CHECK(has_diag(n, "CNT-0007"));
    }
  }
  const ovt_Buffer before = ovt_container_tree(o.container);
  const std::vector<uint8_t> snapshot(before.data, before.data + before.size);  // the handle frees its buffer on expand
  REQUIRE(ovt_container_expand(o.container, offsets[1799]) == OVT_OK);
  const ovt_Buffer after = ovt_container_tree(o.container);
  CHECK(snapshot != std::vector<uint8_t>(after.data, after.data + after.size));
  CHECK(snapshot.size() < after.size);  // a new, larger buffer
  const auto* t = tree_of(o.container);
  for (const auto* n : *t->root()) {
    if (n->fourcc()->string_view() != "moof") {
      continue;
    }
    const bool expanded = n->file_span()->offset() == offsets[1799];
    CHECK((child(n, "traf") != nullptr) == expanded);
    CHECK(has_diag(n, "CNT-0007") == !expanded);
    if (expanded) {
      const auto* traf = child(n, "traf");
      CHECK(child(traf, "tfhd") != nullptr);
      CHECK(child(traf, "tfdt") != nullptr);
      CHECK(child(traf, "trun") != nullptr);
      CHECK(child(n, "mfhd") != nullptr);
    }
  }
  CHECK(ovt_container_expand(o.container, offsets[1799]) == OVT_OK);  // already expanded: no-op
}

TEST_CASE("lazy: at most 64 moofs are decoded eagerly, and equal a full walk", "[container][isobmff][lazy][fast]") {
  for (const unsigned count : {1U, 64U, 65U}) {
    CAPTURE(count);
    const Bytes data = fragmented_file(count);
    Opened o(std::make_unique<ovt::io::MemoryFile>(data));
    const auto* t = tree_of(o.container);
    unsigned with_traf = 0;
    for (const auto* n : *t->root()) {
      if (n->fourcc()->string_view() == "moof" && child(n, "traf") != nullptr) {
        ++with_traf;
      }
    }
    CHECK(with_traf == (count <= 64 ? count : 0U));
    if (count <= 64) {
      const ovt::io::MemoryFile f(data);
      iso::WalkResult full = iso::walk(f);
      ovt::fb::ContainerTreeT expect;
      expect.format = "isobmff";
      expect.root = std::move(full.top_level);
      expect.diagnostics = std::move(full.diagnostics);
      flatbuffers::FlatBufferBuilder fbb;
      fbb.Finish(ovt::fb::ContainerTree::Pack(fbb, &expect));
      const ovt_Buffer b = ovt_container_tree(o.container);
      CHECK(std::vector<uint8_t>(b.data, b.data + b.size) ==
            std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()));
    }
  }
}

TEST_CASE("lazy: expanding every box of a lazy file leaves the same tree as a full walk",
          "[container][isobmff][lazy][fast]") {
  const Bytes data = fragmented_file(70);
  Opened o(std::make_unique<ovt::io::MemoryFile>(data));
  std::vector<uint64_t> all;
  for (const auto* n : *tree_of(o.container)->root()) {
    all.push_back(n->file_span()->offset());
  }
  for (const uint64_t off : all) {
    REQUIRE(ovt_container_expand(o.container, off) == OVT_OK);
  }
  const ovt::io::MemoryFile f(data);
  iso::WalkResult full = iso::walk(f);
  ovt::fb::ContainerTreeT expect;
  expect.format = "isobmff";
  expect.root = std::move(full.top_level);
  expect.diagnostics = std::move(full.diagnostics);
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::ContainerTree::Pack(fbb, &expect));
  const ovt_Buffer b = ovt_container_tree(o.container);
  CHECK(std::vector<uint8_t>(b.data, b.data + b.size) ==
        std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()));
}

TEST_CASE("lazy: expand of an unknown offset is OVT_E_INVALID_ARG, of any other box a no-op",
          "[container][isobmff][lazy][fast]") {
  const Bytes data = fragmented_file(3);
  Opened o(std::make_unique<ovt::io::MemoryFile>(data));
  CHECK(ovt_container_expand(o.container, 3) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_expand(o.container, data.size()) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_expand(o.container, 0) == OVT_OK);  // ftyp
  // Every expand republishes the tree, so read the offset before the first call (the buffer is freed then).
  const uint64_t moov_at = tree_of(o.container)->root()->Get(1)->file_span()->offset();
  CHECK(ovt_container_expand(o.container, moov_at) == OVT_OK);
  CHECK(ovt_container_expand(o.container, moov_at + 8) == OVT_OK);  // mvhd inside moov
}

TEST_CASE("lazy: a moof whose first child is not mfhd keeps no children until expanded",
          "[container][isobmff][lazy][fast]") {
  Bytes data = fragmented_file(65);
  const uint64_t first = moof_offsets(tree_of(Opened(std::make_unique<ovt::io::MemoryFile>(data)).container)).front();
  data[first + 12] = 'x';  // mfhd -> xfhd
  Opened o(std::make_unique<ovt::io::MemoryFile>(data));
  const auto* m = tree_of(o.container)->root()->Get(2);
  REQUIRE(m->fourcc()->string_view() == "moof");
  CHECK(m->children() == nullptr);
  CHECK(has_diag(m, "CNT-0007"));
  REQUIRE(ovt_container_expand(o.container, first) == OVT_OK);
  CHECK(child(tree_of(o.container)->root()->Get(2), "traf") != nullptr);
}

TEST_CASE("lazy: mfro locates mfra from the end of the file", "[container][isobmff][lazy][fast]") {
  Bytes data = fragmented_file(2);
  const uint64_t mfra_at = data.size();
  const Bytes tfra = box("tfra", Bytes(12, 0));
  const Bytes mfro_payload = full_box_payload(static_cast<uint32_t>(8 + tfra.size() + 16));
  put_all(data, box("mfra", cat({tfra, box("mfro", mfro_payload)})));
  {
    const ovt::io::MemoryFile f(data);
    CHECK(iso::locate_mfra(f) == mfra_at);
    ovt::container::Format fmt = ovt::container::Format::Isobmff;
    ovt_status why = OVT_OK;
    auto reader = ovt::container::make_reader(fmt, f, &why);
    REQUIRE(reader != nullptr);
    CHECK(dynamic_cast<iso::IsobmffReader&>(*reader).mfra_offset() == mfra_at);
  }
  SECTION("a wrong mfro size, a wrong mfra type or a short file locate nothing") {
    Bytes bad = data;
    bad[bad.size() - 1] += 1;  // parent_size no longer the mfra size
    CHECK_FALSE(iso::locate_mfra(ovt::io::MemoryFile(bad)).has_value());
    bad = data;
    bad[mfra_at + 4] = 'x';
    CHECK_FALSE(iso::locate_mfra(ovt::io::MemoryFile(bad)).has_value());
    bad = data;
    bad[bad.size() - 12] = 'x';  // mfro -> xfro
    CHECK_FALSE(iso::locate_mfra(ovt::io::MemoryFile(bad)).has_value());
    CHECK_FALSE(iso::locate_mfra(ovt::io::MemoryFile(Bytes(20, 0))).has_value());
  }
}

TEST_CASE("lazy: mfra is found at the end of a huge file without walking the top level first",
          "[container][isobmff][lazy][fast]") {
  const Bytes ftyp = box("ftyp", cat({Bytes{'i', 's', 'o', '6'}, full_box_payload(0)}));
  const Bytes mfro = box("mfro", full_box_payload(8 + 16));
  const Bytes mfra = box("mfra", mfro);
  const uint64_t mdat_at = ftyp.size();
  const uint64_t mfra_at = mdat_at + 4 * kGiB;
  SparseFile f(mfra_at + mfra.size(), {{0, ftyp}, {mdat_at, large_box_header("mdat", 4 * kGiB)}, {mfra_at, mfra}});
  CHECK(iso::locate_mfra(f) == mfra_at);
  CHECK(f.bytes_read() == 16 + 8);
}

TEST_CASE("lazy: garbage that sniffs as nothing useful is OVT_E_CORRUPT", "[container][isobmff][lazy][fast]") {
  const ovt::io::MemoryFile f(Bytes(32, 0x01));
  ovt_status why = OVT_OK;
  CHECK(iso::make_reader(f, &why) == nullptr);
  CHECK(why == OVT_E_CORRUPT);
}
