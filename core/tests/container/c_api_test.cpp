// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Argument and error rules of the container C API (chapter 08 §8.1, WI-1.200): NULL handles and outputs, I/O
// failures and exceptions turned into statuses, and the Reader defaults. Per-format behaviour lives in the
// reader tests (ivf_test.cpp, y4m_test.cpp, ...).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"

namespace {

using Bytes = std::vector<uint8_t>;

enum class Throw { Io, NoMem, Other };

// A RandomAccessFile whose every read throws.
class ThrowingFile final : public ovt::io::RandomAccessFile {
 public:
  explicit ThrowingFile(Throw what) : RandomAccessFile(ovt::io::FileInfo{4096, 0, "throwing.ivf"}), what_(what) {}
  size_t read(uint64_t /*offset*/, size_t /*len*/, uint8_t* /*dst*/) const override { raise(); }
  [[nodiscard]] Holder view(uint64_t /*offset*/, size_t /*len*/) const override { raise(); }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*access*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }

 private:
  [[noreturn]] void raise() const {
    switch (what_) {
      case Throw::Io:
        throw ovt::io::IoError(OVT_E_IO, 5, "injected failure");
      case Throw::NoMem:
        throw std::bad_alloc();
      case Throw::Other:
        break;
    }
    throw std::runtime_error("injected failure");
  }
  Throw what_;
};

// A reader that keeps every Reader default except expand, which throws.
class ThrowingReader final : public ovt::container::Reader {
 public:
  ovt_status expand(uint64_t /*box_offset*/) override { throw ovt::io::IoError(OVT_E_IO, 5, "injected failure"); }
};

}  // namespace

TEST_CASE("container C API: NULL handles and outputs are OVT_E_INVALID_ARG", "[container][open][fast]") {
  ovt_Buffer out{nullptr, 0};
  uint64_t box = 0;
  int depth = 0;
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open(nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_open_manifest("https://example.invalid/a.mpd", nullptr, nullptr) == OVT_E_INVALID_ARG);
  c = reinterpret_cast<ovt_Container*>(0x1);
  CHECK(ovt_container_open_manifest("https://example.invalid/a.mpd", nullptr, &c) == OVT_E_UNSUPPORTED);  // phase 4
  CHECK(c == nullptr);
  CHECK(std::string(ovt_container_format(nullptr)) == "unknown");
  CHECK(ovt_container_tree(nullptr).data == nullptr);
  CHECK(ovt_container_expand(nullptr, 0) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_expand_all(nullptr, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_track_count(nullptr) == 0);
  CHECK(ovt_container_track_info(nullptr, 0, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_sample_table(nullptr, 0, 0, 0, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_fragments(nullptr, 0, nullptr, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_encryption(nullptr, 0, 0, 0, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_hdr_metadata(nullptr, 0, nullptr, &out, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_items(nullptr, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_item_bytes(nullptr, 1, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_validate(nullptr, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_list_diagnostics(nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_bytes(nullptr, 0, 16, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(nullptr, 0, &box, &depth) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_segment_for_offset(nullptr, 0, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_size(nullptr) == 0);
  CHECK(ovt_container_stats_json(nullptr, &out) == OVT_E_INVALID_ARG);
  CHECK(out.data == nullptr);
}

TEST_CASE("container C API: a valid handle with NULL outputs is OVT_E_INVALID_ARG", "[container][open][fast]") {
  const Bytes bytes(16 * 16 * 3 / 2, 0x80);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes)};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open_format(&handle, "raw", "16x16:yuv420p", nullptr, &c) == OVT_OK);
  REQUIRE(c != nullptr);
  uint64_t box = 0;
  int depth = 0;
  CHECK(ovt_container_track_info(c, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_sample_table(c, 0, 0, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_fragments(c, 0, nullptr, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_encryption(c, 0, 0, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_hdr_metadata(c, 0, nullptr, nullptr, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_items(c, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_item_bytes(c, 1, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_validate(c, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_bytes(c, 0, 16, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(c, 0, nullptr, &depth) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(c, 0, &box, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_segment_for_offset(c, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_stats_json(c, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_size(c) == bytes.size());
  ovt_container_close(c);
}

TEST_CASE("container C API: open of a file without a reader object is OVT_E_INVALID_ARG", "[container][open][fast]") {
  ovt_File empty{};  // no RandomAccessFile behind the handle
  auto* c = reinterpret_cast<ovt_Container*>(0x1);
  CHECK(ovt_container_open(&empty, &c) == OVT_E_INVALID_ARG);
  CHECK(c == nullptr);
  CHECK(ovt_container_open_format(&empty, "ivf", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  const Bytes bytes(64, 0x80);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes)};
  CHECK(ovt_container_open_format(&handle, nullptr, nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_open_format(&handle, "raw", "", nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_open_format(&handle, "ivf", nullptr, nullptr, nullptr) == OVT_E_INVALID_ARG);
}

TEST_CASE("container C API: headerless bytes need open_format", "[container][open][fast]") {
  // sniff() says raw only by extension; ovt_container_open refuses raw because the geometry is required.
  const Bytes bytes(64, 0x80);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes, "frames.yuv")};
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open(&handle, &c) == OVT_E_INVALID_ARG);
  CHECK(c == nullptr);
}

TEST_CASE("container C API: exceptions while opening become statuses", "[container][open][fast]") {
  const auto status_of = [](Throw what, bool forced) {
    ovt_File handle{std::make_unique<ThrowingFile>(what)};
    ovt_Container* c = nullptr;
    const ovt_status st =
        forced ? ovt_container_open_format(&handle, "ivf", nullptr, nullptr, &c) : ovt_container_open(&handle, &c);
    CHECK(c == nullptr);
    return st;
  };
  for (const bool forced : {false, true}) {
    INFO("forced " << forced);
    CHECK(status_of(Throw::Io, forced) == OVT_E_IO);
    CHECK(status_of(Throw::NoMem, forced) == OVT_E_NOMEM);
    CHECK(status_of(Throw::Other, forced) == OVT_E_INTERNAL);
  }
}

TEST_CASE("container C API: expand without a reader, and a throwing reader", "[container][open][fast]") {
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(Bytes(8, 0))};
  // The handle is an aggregate (container.hpp); tests build it directly to reach paths no shipped reader takes.
  auto* bare = new ovt_Container{&handle, ovt::container::Format::Ivf, nullptr, {}};
  CHECK(ovt_container_expand(bare, 0) == OVT_E_UNSUPPORTED);
  CHECK(ovt_container_track_count(bare) == 0);
  ovt_container_close(bare);

  auto* throwing = new ovt_Container{&handle, ovt::container::Format::Ivf, std::make_unique<ThrowingReader>(), {}};
  CHECK(ovt_container_expand(throwing, 0) == OVT_E_IO);
  CHECK(ovt_container_track_count(throwing) == 0);
  // The Reader defaults: no raw format, no sample table.
  CHECK(throwing->reader->raw_format().empty());
  CHECK(throwing->reader->sample_table(0, 0, 0).empty());
  ovt_container_close(throwing);
}
