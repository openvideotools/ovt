// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.130: ovt_stream_unit_bytes() (chapter 07 §7.8, backbone §0.6.1). Acceptance test 1: out_raw equals the file
// bytes at the unit's file_span and out_rbsp equals rbsp_from_nal(out_raw). Acceptance test 2: an index at or beyond
// the unit count gives OVT_E_INVALID_ARG.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/index.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using Bytes = std::vector<uint8_t>;

// A committed seed of the index_builder fuzz target without its selector byte.
Bytes seed(std::string_view name) {
  std::ifstream in(std::filesystem::path(OVT_INDEX_BUILDER_SEEDS) / name, std::ios::binary);
  REQUIRE(in.good());
  Bytes b{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(b.size() > 1);
  b.erase(b.begin());
  return b;
}

// The module objects are linked without ovt_buffer_free(): a caller-owned buffer is a new[] array
// (core/src/api/buffer.cpp).
Bytes take(ovt_Buffer& buf) {
  const std::unique_ptr<const uint8_t[]> owner(buf.data);  // NOLINT(*-avoid-c-arrays)
  Bytes out;
  if (buf.data != nullptr) {
    out.assign(buf.data, buf.data + buf.size);
  }
  buf = ovt_Buffer{nullptr, 0};
  return out;
}

struct Opened {
  Opened(const Bytes& bytes, const std::string& name) {
    file.file = std::make_unique<ovt::io::MemoryFile>(bytes, name);
    REQUIRE(ovt_container_open(&file, &container) == OVT_OK);
    REQUIRE(ovt_stream_open(container, 0, &stream) == OVT_OK);
  }
  ~Opened() {
    ovt_stream_close(stream);
    ovt_container_close(container);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  Opened(Opened&&) = delete;
  Opened& operator=(Opened&&) = delete;

  ovt_File file;
  ovt_Container* container = nullptr;
  ovt_Stream* stream = nullptr;
};

}  // namespace

TEST_CASE("ovt_stream_unit_bytes: raw bytes at file_span and the RBSP of every unit", "[api][index][fast]") {
  struct Case {
    const char* seed;
    const char* name;
    bool has_rbsp;
  };
  for (const Case c : {Case{"seed_0_altref_gop.bin", "a.obu", false}, Case{"seed_2_h264_bframes.bin", "a.264", true},
                       Case{"seed_3_h265_open_gop.bin", "a.265", true}}) {
    CAPTURE(c.name);
    const Bytes data = seed(c.seed);
    Opened o(data, c.name);
    ovt_Buffer index_buf{nullptr, 0};
    REQUIRE(ovt_stream_build_index(o.stream, nullptr, &index_buf) == OVT_OK);
    const Bytes index = take(index_buf);
    const auto* si = flatbuffers::GetRoot<fb::StreamIndex>(index.data());
    REQUIRE_FALSE(si->units()->empty());
    for (uint32_t i = 0; i < si->units()->size(); ++i) {
      CAPTURE(i);
      const fb::ByteSpan* span = si->units()->Get(i)->file_span();
      const std::span<const uint8_t> want(data.data() + span->offset(), span->length());
      ovt_Buffer raw{nullptr, 0};
      ovt_Buffer rbsp{nullptr, 0};
      REQUIRE(ovt_stream_unit_bytes(o.stream, i, &raw, &rbsp) == OVT_OK);
      const Bytes raw_bytes = take(raw);
      const Bytes rbsp_bytes = take(rbsp);
      CHECK(Bytes(want.begin(), want.end()) == raw_bytes);
      if (c.has_rbsp) {
        CHECK(ovt::rbsp_from_nal(raw_bytes).rbsp == rbsp_bytes);
      } else {
        CHECK(rbsp_bytes.empty());
      }
    }
  }
}

TEST_CASE("ovt_stream_unit_bytes: either output may be NULL", "[api][index][fast]") {
  const Bytes data = seed("seed_2_h264_bframes.bin");
  Opened o(data, "a.264");
  ovt_Buffer index_buf{nullptr, 0};
  REQUIRE(ovt_stream_build_index(o.stream, nullptr, &index_buf) == OVT_OK);
  take(index_buf);
  ovt_Buffer raw{nullptr, 0};
  CHECK(ovt_stream_unit_bytes(o.stream, 0, &raw, nullptr) == OVT_OK);
  CHECK_FALSE(take(raw).empty());
  ovt_Buffer rbsp{nullptr, 0};
  CHECK(ovt_stream_unit_bytes(o.stream, 0, nullptr, &rbsp) == OVT_OK);
  CHECK_FALSE(take(rbsp).empty());
  CHECK(ovt_stream_unit_bytes(o.stream, 0, nullptr, nullptr) == OVT_OK);
}

TEST_CASE("ovt_stream_unit_bytes: invalid arguments", "[api][index][fast]") {
  ovt_Buffer raw{reinterpret_cast<const uint8_t*>(1), 1};  // NOLINT(*-reinterpret-cast): must be reset, not read
  CHECK(ovt_stream_unit_bytes(nullptr, 0, &raw, nullptr) == OVT_E_INVALID_ARG);
  CHECK(raw.data == nullptr);

  const Bytes data = seed("seed_2_h264_bframes.bin");
  Opened o(data, "a.264");
  CHECK(ovt_stream_unit_bytes(o.stream, 0, &raw, nullptr) == OVT_E_INVALID_ARG);  // before build_index
  ovt_Buffer index_buf{nullptr, 0};
  REQUIRE(ovt_stream_build_index(o.stream, nullptr, &index_buf) == OVT_OK);
  const Bytes index = take(index_buf);
  const uint32_t units = flatbuffers::GetRoot<fb::StreamIndex>(index.data())->units()->size();
  ovt_Buffer rbsp{nullptr, 0};
  CHECK(ovt_stream_unit_bytes(o.stream, units, &raw, &rbsp) == OVT_E_INVALID_ARG);
  CHECK(raw.data == nullptr);
  CHECK(rbsp.data == nullptr);
  CHECK(ovt_stream_unit_bytes(o.stream, UINT32_MAX, &raw, &rbsp) == OVT_E_INVALID_ARG);
}
