// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/syntax.h (ovt_fb_to_json, ovt_json_to_fb) and ovt/validate.h (ovt_validate_frame_analysis,
// ovt_validate_stream_index) through the shared library (#673, #674): the byte-identical JSON round trip that
// `ovt cache import` relies on (chapter 12 §12.2.13), the DiagnosticList form of a violation, the status codes
// of the two headers and the caller-owned buffers of chapter 12 §12.3.1 rule 3a.

#include <flatbuffers/flatbuffers.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/status.h"
#include "ovt/syntax.h"
#include "ovt/validate.h"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using Bytes = std::vector<std::uint8_t>;

std::string read_golden(const std::string& name) {
  std::ifstream in(std::filesystem::path(OVT_GOLDEN_DIR) / name, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Bytes fixture(const std::string& name) {
  const std::string text = read_golden(name);
  return {text.begin(), text.end()};
}

/// The stream hash the §6.10 fixture's PlaneRef.blob_id values carry (kFixtureSourceHash of mutate.hpp, which
/// this test does not include: it links the shared library only).
Bytes fixture_source_hash() {
  constexpr std::string_view kHex = "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0";
  const auto nibble = [](char ch) { return static_cast<std::uint8_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10); };
  Bytes hash(32);
  for (std::size_t i = 0; i < hash.size(); ++i) {
    hash[i] = static_cast<std::uint8_t>((nibble(kHex[2 * i]) << 4U) | nibble(kHex[(2 * i) + 1]));
  }
  return hash;
}

/// A caller-owned buffer released on scope exit, as a C caller would with ovt_buffer_free.
struct Owned {
  ovt_Buffer buf{nullptr, 0};
  Owned() = default;
  ~Owned() { ovt_buffer_free(&buf); }
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  Owned(Owned&&) = delete;
  Owned& operator=(Owned&&) = delete;
  [[nodiscard]] std::string_view text() const { return {reinterpret_cast<const char*>(buf.data), buf.size}; }
  [[nodiscard]] Bytes bytes() const { return {buf.data, buf.data + buf.size}; }
};

/// The verified DiagnosticList of a validate call.
const fb::DiagnosticList* diagnostics(const ovt_Buffer& buf) {
  REQUIRE(buf.data != nullptr);
  REQUIRE(std::memcmp(flatbuffers::GetBufferIdentifier(buf.data), "OVTD", 4) == 0);
  flatbuffers::Verifier verifier(buf.data, buf.size);
  REQUIRE(verifier.VerifyBuffer<fb::DiagnosticList>("OVTD"));
  const auto* list = flatbuffers::GetRoot<fb::DiagnosticList>(buf.data);
  REQUIRE(list->diagnostics() != nullptr);
  return list;
}

std::size_t count(const ovt_Buffer& buf) {
  return diagnostics(buf)->diagnostics()->size();
}

}  // namespace

TEST_CASE("ovt_json_to_fb + ovt_fb_to_json round-trip the golden JSON byte for byte", "[model][c_api][golden]") {
  const std::string golden = read_golden("tiny16_hevc.json");
  Owned fbuf;
  REQUIRE(ovt_json_to_fb(golden.c_str(), "FrameAnalysis", &fbuf.buf) == OVT_OK);
  REQUIRE(fbuf.buf.data != nullptr);
  CHECK(std::memcmp(flatbuffers::GetBufferIdentifier(fbuf.buf.data), "OVTA", 4) == 0);

  // The import is the canonical fixture (from_json(to_json(b)) == canonicalize(b), chapter 06 §6.8.1) and valid.
  Owned diag;
  REQUIRE(ovt_validate_frame_analysis(fbuf.buf.data, fbuf.buf.size, nullptr, -1, &diag.buf) == OVT_OK);
  CHECK(count(diag.buf) == 0);

  Owned text;
  REQUIRE(ovt_fb_to_json(fbuf.buf.data, fbuf.buf.size, "ovt.fb.FrameAnalysis", 1, &text.buf) == OVT_OK);
  CHECK(text.text() == golden);
  CHECK(text.buf.data[text.buf.size] == 0);  // NUL past size: readable as a C string

  // A second import of the export gives the same bytes again (determinism, §0.7.5).
  Owned again;
  REQUIRE(ovt_json_to_fb(reinterpret_cast<const char*>(text.buf.data), "FrameAnalysis", &again.buf) == OVT_OK);
  CHECK(again.bytes() == fbuf.bytes());

  // Compact text is one line.
  Owned compact;
  REQUIRE(ovt_fb_to_json(fbuf.buf.data, fbuf.buf.size, "FrameAnalysis", 0, &compact.buf) == OVT_OK);
  CHECK(compact.text().find('\n') == std::string_view::npos);
}

TEST_CASE("ovt_json_to_fb reaches the secondary root tables and the other schemas", "[model][c_api][golden]") {
  const std::string index_json = read_golden("tiny16_hevc.index.json");
  Owned index;
  REQUIRE(ovt_json_to_fb(index_json.c_str(), "StreamIndex", &index.buf) == OVT_OK);
  // No identifier for a secondary root table (ovt::model::from_json), and the validator accepts that form.
  Owned diag;
  REQUIRE(ovt_validate_stream_index(index.buf.data, index.buf.size, &diag.buf) == OVT_OK);
  CHECK(count(diag.buf) == 0);
  Owned text;
  REQUIRE(ovt_fb_to_json(index.buf.data, index.buf.size, "StreamIndex", 1, &text.buf) == OVT_OK);
  CHECK(text.text() == index_json);

  Owned metrics;
  REQUIRE(ovt_json_to_fb("{\"version\": \"0.1.0\"}", "ovt.fb.MetricsReport", &metrics.buf) == OVT_OK);
  CHECK(std::memcmp(flatbuffers::GetBufferIdentifier(metrics.buf.data), "OVTR", 4) == 0);
  Owned container;
  REQUIRE(ovt_json_to_fb("{}", "ContainerRoot", &container.buf) == OVT_OK);
  CHECK(std::memcmp(flatbuffers::GetBufferIdentifier(container.buf.data), "OVTC", 4) == 0);
}

TEST_CASE("ovt_json_to_fb / ovt_fb_to_json status codes and zeroed outputs", "[model][c_api][fast]") {
  ovt_Buffer out{reinterpret_cast<const std::uint8_t*>("x"), 1};
  CHECK(ovt_json_to_fb("{}", "NoSuchTable", &out) == OVT_E_INVALID_ARG);
  CHECK(out.data == nullptr);
  CHECK(out.size == 0);
  out = {reinterpret_cast<const std::uint8_t*>("x"), 1};
  CHECK(ovt_json_to_fb("{}", "", &out) == OVT_E_INVALID_ARG);
  CHECK(out.data == nullptr);
  CHECK(ovt_json_to_fb("{}", "ovt.fb.", &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_json_to_fb(nullptr, "FrameAnalysis", &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_json_to_fb("{}", nullptr, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_json_to_fb("{}", "FrameAnalysis", nullptr) == OVT_E_INVALID_ARG);

  out = {reinterpret_cast<const std::uint8_t*>("x"), 1};
  CHECK(ovt_json_to_fb("{\"schema_version\": ", "FrameAnalysis", &out) == OVT_E_CORRUPT);
  CHECK(out.data == nullptr);
  CHECK(out.size == 0);
  CHECK(ovt_json_to_fb("{\"source_hash\": \"abc\"}", "StreamIndex", &out) == OVT_E_CORRUPT);  // odd hex
  CHECK(ovt_json_to_fb("[1, 2]", "FrameAnalysis", &out) == OVT_E_CORRUPT);

  const Bytes fa = fixture("tiny16_hevc.fa");
  out = {reinterpret_cast<const std::uint8_t*>("x"), 1};
  CHECK(ovt_fb_to_json(fa.data(), fa.size(), "StreamIndex", 1, &out) == OVT_E_CORRUPT);  // wrong root
  CHECK(out.data == nullptr);
  CHECK(ovt_fb_to_json(fa.data(), 7, "FrameAnalysis", 1, &out) == OVT_E_CORRUPT);  // truncated
  CHECK(ovt_fb_to_json(fa.data(), fa.size(), "NoSuchTable", 1, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_fb_to_json(nullptr, 0, "FrameAnalysis", 1, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_fb_to_json(fa.data(), fa.size(), nullptr, 1, &out) == OVT_E_INVALID_ARG);
  CHECK(ovt_fb_to_json(fa.data(), fa.size(), "FrameAnalysis", 1, nullptr) == OVT_E_INVALID_ARG);
}

TEST_CASE("ovt_validate_frame_analysis reports violations as MDL-VAL diagnostics", "[model][c_api][golden]") {
  const Bytes fa = fixture("tiny16_hevc.fa");

  SECTION("the golden fixture is clean, with and without an expected hash") {
    Owned diag;
    REQUIRE(ovt_validate_frame_analysis(fa.data(), fa.size(), nullptr, -1, &diag.buf) == OVT_OK);
    CHECK(count(diag.buf) == 0);
    const Bytes hash = fixture_source_hash();
    Owned with_hash;
    REQUIRE(ovt_validate_frame_analysis(fa.data(), fa.size(), hash.data(), -1, &with_hash.buf) == OVT_OK);
    CHECK(count(with_hash.buf) == 0);
    Owned with_codec;
    REQUIRE(ovt_validate_frame_analysis(fa.data(), fa.size(), nullptr, static_cast<int>(fb::Codec::H265),
                                        &with_codec.buf) == OVT_OK);
    CHECK(count(with_codec.buf) == 0);
  }

  SECTION("a truncated buffer is V-001 as MDL-VAL-0001, not a status code") {
    Owned diag;
    REQUIRE(ovt_validate_frame_analysis(fa.data(), 16, nullptr, -1, &diag.buf) == OVT_OK);
    const auto* list = diagnostics(diag.buf);
    REQUIRE(list->diagnostics()->size() == 1);
    const auto* d = list->diagnostics()->Get(0);
    CHECK(d->code()->string_view() == "MDL-VAL-0001");
    CHECK(d->severity() == fb::Severity::Error);
    CHECK(d->message()->string_view().starts_with("V-001: "));
    CHECK(d->spec_ref()->string_view() == "chapter 06 §6.9");
    Owned empty;
    REQUIRE(ovt_validate_frame_analysis(fa.data(), 0, nullptr, -1, &empty.buf) == OVT_OK);
    CHECK(count(empty.buf) == 1);
  }

  SECTION("a wrong expected hash fires V-023 with its path in the message") {
    const Bytes other(32, 0xAB);
    Owned diag;
    REQUIRE(ovt_validate_frame_analysis(fa.data(), fa.size(), other.data(), -1, &diag.buf) == OVT_OK);
    const auto* list = diagnostics(diag.buf);
    REQUIRE(!list->diagnostics()->empty());
    bool found = false;
    for (const auto* d : *list->diagnostics()) {
      if (d->code()->string_view() == "MDL-VAL-0023") {
        found = true;
        CHECK(d->message()->string_view().starts_with("V-023: "));
        CHECK(d->message()->string_view().ends_with(")"));
        CHECK(d->message()->string_view().find(" (at ") != std::string_view::npos);
      }
    }
    CHECK(found);
  }

  SECTION("argument checks") {
    ovt_Buffer out{reinterpret_cast<const std::uint8_t*>("x"), 1};
    CHECK(ovt_validate_frame_analysis(nullptr, 0, nullptr, -1, &out) == OVT_E_INVALID_ARG);
    CHECK(out.data == nullptr);
    CHECK(out.size == 0);
    CHECK(ovt_validate_frame_analysis(fa.data(), fa.size(), nullptr, -1, nullptr) == OVT_E_INVALID_ARG);
    CHECK(ovt_validate_frame_analysis(fa.data(), fa.size(), nullptr, -2, &out) == OVT_E_INVALID_ARG);
    CHECK(ovt_validate_frame_analysis(fa.data(), fa.size(), nullptr, static_cast<int>(fb::Codec::MAX) + 1, &out) ==
          OVT_E_INVALID_ARG);
    CHECK(out.data == nullptr);
  }
}

TEST_CASE("ovt_validate_stream_index: V-001 for garbage, argument checks", "[model][c_api][fast]") {
  const Bytes garbage(40, 0x5A);
  Owned diag;
  REQUIRE(ovt_validate_stream_index(garbage.data(), garbage.size(), &diag.buf) == OVT_OK);
  const auto* list = diagnostics(diag.buf);
  REQUIRE(list->diagnostics()->size() == 1);
  CHECK(list->diagnostics()->Get(0)->code()->string_view() == "MDL-VAL-0001");
  CHECK(list->diagnostics()->Get(0)->message()->string_view().starts_with("V-001: "));

  ovt_Buffer out{reinterpret_cast<const std::uint8_t*>("x"), 1};
  CHECK(ovt_validate_stream_index(nullptr, 0, &out) == OVT_E_INVALID_ARG);
  CHECK(out.data == nullptr);
  CHECK(out.size == 0);
  CHECK(ovt_validate_stream_index(garbage.data(), garbage.size(), nullptr) == OVT_E_INVALID_ARG);
}
