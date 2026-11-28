// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of ovt/index.h (chapter 12 §12.3.8, WI-1.121): ovt_stream_open() over the formats with a unit map,
// ovt_stream_build_index() with its source_hash (chapter 6 §6.2.8) and the random access map (§7.4.2 S11), and
// acceptance test 3: a truncated file still gives OVT_OK, a Diagnostic and every complete frame.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

#include "index/rap_map.h"
#include "index/source_hash.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/model/validator.hpp"
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

void put_le(Bytes& out, uint64_t v, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) {
    out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
}

// An IVF file (AV01, 16x16, 1/25 s per tick) with one frame per entry of `frames`.
Bytes make_ivf(const std::vector<Bytes>& frames) {
  Bytes out = {'D', 'K', 'I', 'F'};
  put_le(out, 0, 2);
  put_le(out, 32, 2);
  out.insert(out.end(), {'A', 'V', '0', '1'});
  put_le(out, 16, 2);
  put_le(out, 16, 2);
  put_le(out, 25, 4);  // rate
  put_le(out, 1, 4);   // scale
  put_le(out, frames.size(), 4);
  put_le(out, 0, 4);
  for (size_t i = 0; i < frames.size(); ++i) {
    put_le(out, frames[i].size(), 4);
    put_le(out, i, 8);
    out.insert(out.end(), frames[i].begin(), frames[i].end());
  }
  return out;
}

// Splits a Section 5 stream into temporal units (each starts with a temporal delimiter OBU: 12 00).
std::vector<Bytes> temporal_units(const Bytes& obu) {
  std::vector<Bytes> out;
  for (size_t i = 0; i < obu.size(); ++i) {
    if (i + 1 < obu.size() && obu[i] == 0x12 && obu[i + 1] == 0x00) {
      out.emplace_back();
    }
    REQUIRE_FALSE(out.empty());
    out.back().push_back(obu[i]);
  }
  return out;
}

// A container and a stream over in-memory bytes; `name` gives the format sniffer its extension hint.
struct Opened {
  Opened(std::span<const uint8_t> bytes, const std::string& name) {
    file.file = std::make_unique<ovt::io::MemoryFile>(bytes, name);
    container_status = ovt_container_open(&file, &container);
    if (container_status == OVT_OK) {
      stream_status = ovt_stream_open(container, 0, &stream);
    }
  }
  ~Opened() {
    ovt_stream_close(stream);
    ovt_container_close(container);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  Opened(Opened&&) = delete;
  Opened& operator=(Opened&&) = delete;

  // Builds the index and returns a validated copy.
  Bytes build() {
    REQUIRE(stream != nullptr);
    ovt_Buffer buf{nullptr, 0};
    REQUIRE(ovt_stream_build_index(stream, nullptr, &buf) == OVT_OK);
    // This test links the module objects, not the shared library that exports ovt_buffer_free(): a caller-owned
    // buffer is a new[] array (core/src/api/buffer.cpp), released here the way ovt_buffer_free() does.
    const std::unique_ptr<const uint8_t[]> owner(buf.data);  // NOLINT(*-avoid-c-arrays)
    const std::span<const uint8_t> bytes(buf.data, buf.size);
    Bytes index(bytes.begin(), bytes.end());
    for (const auto& v : ovt::model::validate_stream_index(index)) {
      FAIL_CHECK(v.rule << ": " << v.message);
    }
    return index;
  }

  ovt_File file;
  ovt_Container* container = nullptr;
  ovt_Stream* stream = nullptr;
  ovt_status container_status = OVT_E_INTERNAL;
  ovt_status stream_status = OVT_E_INTERNAL;
};

const fb::StreamIndex& root(const Bytes& index) {
  return *flatbuffers::GetRoot<fb::StreamIndex>(index.data());
}

bool has_code(const fb::StreamIndex& si, std::string_view code) {
  return si.diagnostics() != nullptr && std::ranges::any_of(*si.diagnostics(), [code](const fb::Diagnostic* d) {
           return d->code()->string_view() == code;
         });
}

std::string hex(std::span<const uint8_t> bytes) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : bytes) {
    out += kDigits[b >> 4U];
    out += kDigits[b & 15U];
  }
  return out;
}

}  // namespace

TEST_CASE("source_hash: SHA-256 of prefix, size and mtime", "[index][fast]") {
  // FIPS 180-4 examples: "abc" and the 56-byte message that needs a second padding block.
  const std::string_view abc = "abc";
  ovt::index::Sha256 a;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): text as bytes
  a.update({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()});
  CHECK(hex(a.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string_view two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  ovt::index::Sha256 b;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): text as bytes
  b.update({reinterpret_cast<const uint8_t*>(two.data()), two.size()});
  CHECK(hex(b.finish()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex(ovt::index::Sha256().finish()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // §6.2.8: file[0 : min(size, 1 MiB)] ∥ LE64(size) ∥ LE64(mtime_ns). A MemoryFile has mtime_ns 0.
  Bytes data((size_t{1} << 20U) + 5, 0x5A);
  data[3] = 1;
  ovt::index::Sha256 want;
  want.update(std::span(data).first(size_t{1} << 20U));
  Bytes tail;
  put_le(tail, data.size(), 8);
  put_le(tail, 0, 8);
  want.update(tail);
  const auto expected = want.finish();
  CHECK(ovt::index::source_hash(ovt::io::MemoryFile(data)) == expected);
  data.back() = 0;  // beyond the hashed prefix: the hash does not see it
  CHECK(ovt::index::source_hash(ovt::io::MemoryFile(data)) == expected);
  data.pop_back();  // the size is hashed
  CHECK(ovt::index::source_hash(ovt::io::MemoryFile(data)) != expected);
}

TEST_CASE("stream C API: NULL arguments and functions of later work items", "[index][api][fast]") {
  ovt_Stream* stream = nullptr;
  ovt_Buffer buf{nullptr, 0};
  CHECK(ovt_stream_open(nullptr, 0, &stream) == OVT_E_INVALID_ARG);
  CHECK(ovt_stream_build_index(nullptr, nullptr, &buf) == OVT_E_INVALID_ARG);
  CHECK(ovt_stream_codec(nullptr) == 0);
  CHECK(ovt_stream_index(nullptr).data == nullptr);
  CHECK(ovt_stream_rap_before(nullptr, 0) == -1);
  ovt_stream_close(nullptr);

  const Bytes obu = seed("seed_0_altref_gop.bin");
  Opened o(obu, "stream.obu");
  REQUIRE(o.stream_status == OVT_OK);
  CHECK(ovt_stream_open(o.container, 1, &stream) == OVT_E_INVALID_ARG);  // one track
  CHECK(ovt_stream_open(o.container, 0, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_stream_build_index(o.stream, nullptr, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_stream_index(o.stream).data == nullptr);  // before build_index
  CHECK(ovt_stream_rap_before(o.stream, 0) == -1);
  ovt_ColorDescription color{};
  CHECK(ovt_stream_index_summary(o.stream, &buf) == OVT_E_UNSUPPORTED);
  CHECK(ovt_stream_unit_syntax(o.stream, 0, &buf) == OVT_E_INVALID_ARG);  // before build_index (WI-1.125)
  CHECK(ovt_stream_frame_info(o.stream, 0, &buf) == OVT_E_UNSUPPORTED);
  CHECK(ovt_stream_color(o.stream, &color) == OVT_E_UNSUPPORTED);
  CHECK(ovt_stream_extract(o.stream, nullptr, 0, "obu", &buf) == OVT_E_UNSUPPORTED);
  CHECK(ovt_stream_min_cvs_units(o.stream, 0, 0, &buf) == OVT_E_UNSUPPORTED);
  CHECK(ovt_stream_frame_count(o.stream) == -1);
  CHECK(ovt_stream_display_to_decode(o.stream, 0) == -1);
  CHECK(ovt_stream_decode_to_display(o.stream, 0) == -1);

  // A format without a unit map yet.
  const std::string_view y4m = "YUV4MPEG2 W2 H2 F25:1 Ip A1:1 C420\nFRAME\n\x10\x10\x10\x10\x80\x80";
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): text as bytes
  Opened other({reinterpret_cast<const uint8_t*>(y4m.data()), y4m.size()}, "clip.y4m");
  REQUIRE(other.container_status == OVT_OK);
  CHECK(other.stream_status == OVT_E_UNSUPPORTED);
  CHECK(other.stream == nullptr);
}

TEST_CASE("stream C API: Annex B and OBU streams are indexed with their codec, hash and RAP map",
          "[index][api][fast]") {
  struct Case {
    const char* seed;
    const char* name;
    fb::Codec codec;
    uint32_t frames;
    std::array<int, 3> rap_before;  // of decode index 0, 5 and 7
  };
  const std::array<Case, 3> cases = {{{"seed_0_altref_gop.bin", "a.obu", fb::Codec::AV1, 9, {0, 0, 0}},
                                      {"seed_2_h264_bframes.bin", "a.264", fb::Codec::H264, 12, {0, 0, 6}},
                                      {"seed_3_h265_open_gop.bin", "a.265", fb::Codec::H265, 12, {0, 4, 4}}}};
  for (const Case& c : cases) {
    CAPTURE(c.name);
    const Bytes data = seed(c.seed);
    Opened o(data, c.name);
    REQUIRE(o.stream_status == OVT_OK);
    CHECK(ovt_stream_codec(o.stream) == static_cast<uint8_t>(c.codec));
    const Bytes index = o.build();
    const fb::StreamIndex& si = root(index);
    CHECK(si.codec() == c.codec);
    CHECK(si.frames()->size() == c.frames);
    const auto hash = ovt::index::source_hash(ovt::io::MemoryFile(data));
    REQUIRE(si.source_hash()->size() == hash.size());
    CHECK(std::equal(hash.begin(), hash.end(), si.source_hash()->begin()));
    // The handle keeps the same bytes, and a second build gives them again (§0.7.5).
    const ovt_Buffer kept = ovt_stream_index(o.stream);
    REQUIRE(kept.size == index.size());
    CHECK(std::equal(index.begin(), index.end(), kept.data));
    CHECK(o.build() == index);
    CHECK(ovt_stream_rap_before(o.stream, 0) == c.rap_before[0]);
    CHECK(ovt_stream_rap_before(o.stream, 5) == c.rap_before[1]);
    CHECK(ovt_stream_rap_before(o.stream, 7) == c.rap_before[2]);
    CHECK(ovt_stream_rap_before(o.stream, -1) == -1);
    CHECK(ovt_stream_rap_before(o.stream, static_cast<int>(c.frames)) == -1);
  }
}

TEST_CASE("rap_map: RASL pictures after a CRA are skipped when decoding starts there", "[index][h265][fast]") {
  const Bytes data = seed("seed_3_h265_open_gop.bin");
  Opened o(data, "a.265");
  REQUIRE(o.stream_status == OVT_OK);
  const Bytes index = o.build();
  const ovt::index::RapMap raps(root(index));
  REQUIRE(raps.size() == 12);
  // Decode order: IDR P B B | CRA RASL RASL P B B P B. The two leading pictures of the CRA reference the P
  // picture before it.
  for (int d = 0; d < 12; ++d) {
    CAPTURE(d);
    CHECK(raps.skip_when_starting_at_rap(d) == (d == 5 || d == 6));
  }
  CHECK_FALSE(raps.skip_when_starting_at_rap(12));
  CHECK(ovt::index::RapMap().rap_before(0) == -1);
}

// AT3: Given a truncated file (last unit cut), When indexed, Then OVT_OK with Diagnostic IO-0001 / SYN-AV1-0101
// and all complete frames present.
TEST_CASE("index_truncated_file: OVT_OK, a diagnostic and every complete frame", "[index][api][fast]") {
  const Bytes obu = seed("seed_0_altref_gop.bin");
  Opened whole_obu(obu, "whole.obu");
  REQUIRE(whole_obu.stream_status == OVT_OK);
  const Bytes whole_index = whole_obu.build();
  const uint32_t frames = root(whole_index).frames()->size();
  REQUIRE(frames == 9);
  CHECK_FALSE(has_code(root(whole_index), "SYN-GEN-0105"));
  CHECK_FALSE(has_code(root(whole_index), "SYN-AV1-0101"));

  SECTION("IVF: the last frame's payload is cut") {
    const std::vector<Bytes> units = temporal_units(obu);
    REQUIRE(units.size() == 7);
    const Bytes ivf = make_ivf(units);
    Opened whole(ivf, "whole.ivf");
    REQUIRE(whole.stream_status == OVT_OK);
    CHECK(ovt_stream_codec(whole.stream) == static_cast<uint8_t>(fb::Codec::AV1));
    const Bytes ivf_index = whole.build();
    REQUIRE(root(ivf_index).frames()->size() == frames);
    CHECK(root(ivf_index).frames()->Get(0)->timebase_den() == 25);  // container timing
    CHECK(root(ivf_index).frames()->Get(frames - 1)->pts() == 6);

    Opened cut(std::span<const uint8_t>(ivf).first(ivf.size() - 5), "cut.ivf");
    REQUIRE(cut.stream_status == OVT_OK);
    const Bytes index = cut.build();
    const fb::StreamIndex& si = root(index);
    CHECK(has_code(si, "IO-0001"));
    // The last temporal unit holds one frame; the eight before it are complete.
    REQUIRE(si.frames()->size() >= frames - 1);
    for (uint32_t i = 0; i + 1 < frames; ++i) {
      CAPTURE(i);
      CHECK(si.frames()->Get(i)->size_bytes() == root(ivf_index).frames()->Get(i)->size_bytes());
      CHECK(si.frames()->Get(i)->type() == root(ivf_index).frames()->Get(i)->type());
    }
  }
  SECTION("OBU stream: the frame OBU of the last but one temporal unit is cut") {
    // The stream ends with a 19-byte frame OBU, a temporal delimiter and a 3-byte show_existing_frame header.
    Opened cut(std::span<const uint8_t>(obu).first(obu.size() - 8), "cut.obu");
    REQUIRE(cut.stream_status == OVT_OK);
    const Bytes index = cut.build();
    const fb::StreamIndex& si = root(index);
    CHECK((has_code(si, "SYN-AV1-0101") || has_code(si, "SYN-GEN-0105")));
    REQUIRE(si.frames()->size() >= frames - 2);
    for (uint32_t i = 0; i + 2 < frames; ++i) {
      CAPTURE(i);
      CHECK(si.frames()->Get(i)->size_bytes() == root(whole_index).frames()->Get(i)->size_bytes());
    }
  }
  SECTION("Annex B: the last slice is cut inside its header") {
    for (const char* name : {"seed_2_h264_bframes.bin", "seed_3_h265_open_gop.bin"}) {
      CAPTURE(name);
      const Bytes data = seed(name);
      const bool h264 = std::string_view(name).find("h264") != std::string_view::npos;
      Opened cut(std::span<const uint8_t>(data).first(data.size() - 5), h264 ? "cut.264" : "cut.265");
      REQUIRE(cut.stream_status == OVT_OK);
      const Bytes index = cut.build();
      CHECK(has_code(root(index), "BR-0001"));  // the reader ran off the unit: it belongs to no frame
      CHECK(root(index).frames()->size() == 11);
    }
  }
}
