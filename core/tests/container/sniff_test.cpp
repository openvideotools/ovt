// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the format sniffer, the container handle and the CNT-* registry (chapter 08 §8.6.1, WI-1.200).

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/format.hpp"
#include "io/ovt_file.hpp"
#include "test_support/corpus.h"

namespace {

using ovt::container::AnnexBCodec;
using ovt::container::Format;
using Bytes = std::vector<uint8_t>;

Bytes make(std::initializer_list<uint8_t> head, size_t total = 0) {
  Bytes b(head);
  if (b.size() < total) {
    b.resize(total, 0xAA);
  }
  return b;
}

Bytes box(uint32_t size, const char* type, size_t total = 32) {
  Bytes b = {static_cast<uint8_t>(size >> 24U), static_cast<uint8_t>(size >> 16U), static_cast<uint8_t>(size >> 8U),
             static_cast<uint8_t>(size)};
  b.insert(b.end(), type, type + 4);
  b.resize(total, 0x11);
  return b;
}

ovt::container::SniffResult sniff(const Bytes& b, std::string_view ext = {}) {
  return ovt::container::sniff_bytes(b, b.size(), ext);
}

// A file in the test binary directory, removed on destruction.
class TempFile {
 public:
  TempFile(const std::string& name, const Bytes& content)
      : path_(std::filesystem::path(OVT_TEST_BINARY_DIR) / (std::to_string(::getpid()) + "_" + name)) {
    std::ofstream(path_, std::ios::binary)
        .write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
  }
  ~TempFile() { std::filesystem::remove(path_); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("sniff: ISOBMFF first-box variants", "[container][sniff][fast]") {
  CHECK(sniff(box(24, "ftyp")).format == Format::Isobmff);
  CHECK(sniff(box(24, "moov")).format == Format::Isobmff);  // QuickTime moov-first
  CHECK(sniff(box(24, "wide")).format == Format::Isobmff);
  CHECK(sniff(box(24, "free")).format == Format::Isobmff);
  CHECK(sniff(box(24, "styp")).format == Format::Isobmff);  // segment
  CHECK(sniff(box(0, "mdat")).format == Format::Isobmff);   // size 0: to end of file
  CHECK(sniff(box(1, "mdat")).format == Format::Isobmff);   // largesize follows
  CHECK(sniff(box(4, "ftyp")).format != Format::Isobmff);   // size 2..7 is not a box header
  CHECK(sniff(box(24, "abcd")).format == Format::Unknown);
}

TEST_CASE("sniff: signatures", "[container][sniff][fast]") {
  CHECK(sniff(make({'D', 'K', 'I', 'F'}, 32)).format == Format::Ivf);
  const std::string y4m = "YUV4MPEG2 W16 H16 F25:1\n";
  CHECK(sniff(Bytes(y4m.begin(), y4m.end())).format == Format::Y4m);
  CHECK(sniff(make({0x1A, 0x45, 0xDF, 0xA3}, 32)).format == Format::Ebml);
  CHECK(sniff(make({0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05}, 32)).format == Format::Mxf);

  Bytes run_in(100, 0);
  const Bytes key = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0D, 0x01, 0x02, 0x01};
  run_in.insert(run_in.end(), key.begin(), key.end());
  run_in.resize(300, 0x55);
  CHECK(sniff(run_in).format == Format::Mxf);

  const std::string hls = "#EXTM3U\n#EXT-X-VERSION:7\n";
  CHECK(sniff(Bytes(hls.begin(), hls.end())).format == Format::Hls);
  const std::string mpd = "<?xml version=\"1.0\"?>\n<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\">";
  CHECK(sniff(Bytes(mpd.begin(), mpd.end())).format == Format::Dash);
  const std::string xml = "<?xml version=\"1.0\"?>\n<other/>";
  CHECK(sniff(Bytes(xml.begin(), xml.end())).format == Format::Unknown);
}

TEST_CASE("sniff: MPEG-TS strides", "[container][sniff][fast]") {
  for (const size_t stride : {size_t{188}, size_t{192}, size_t{204}}) {
    Bytes b(stride * 3 + 4, 0);
    b[0] = 0x47;
    b[stride] = 0x47;
    b[2 * stride] = 0x47;
    CHECK(sniff(b).format == Format::Mpegts);
  }
  Bytes bad(600, 0);
  bad[0] = 0x47;
  bad[188] = 0x47;  // third sync byte missing
  CHECK(sniff(bad).format != Format::Mpegts);
}

TEST_CASE("sniff: Annex B and its codec guess", "[container][sniff][fast]") {
  auto r = sniff(make({0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28}, 32));  // H.264 SPS
  CHECK(r.format == Format::AnnexB);
  CHECK(r.annexb_codec == AnnexBCodec::H264);

  r = sniff(make({0, 0, 1, 0x40, 0x01, 0x0C, 0x01}, 32));  // H.265 VPS (type 32), temporal_id_plus1 = 1
  CHECK(r.format == Format::AnnexB);
  CHECK(r.annexb_codec == AnnexBCodec::H265);

  r = sniff(make({0, 0, 1, 0x41, 0x01, 0x00}, 32));  // 0x41: H.264 slice (type 1) and H.265 VPS (type 32)
  CHECK(r.format == Format::AnnexB);
  CHECK(r.annexb_codec == AnnexBCodec::Ambiguous);

  Bytes zeros(64, 0);  // up to 64 leading zero bytes are tolerated
  zeros.insert(zeros.end(), {0, 0, 1, 0x67, 0x42});
  CHECK(sniff(zeros).format == Format::AnnexB);

  Bytes too_many(70, 0);
  too_many.insert(too_many.end(), {1, 0x67, 0x42});
  CHECK(sniff(too_many).format != Format::AnnexB);
}

TEST_CASE("sniff: AV1 OBU streams", "[container][sniff][fast]") {
  // Section 5: temporal delimiter (0x12 0x00) then a sequence header with a 2-byte payload.
  CHECK(sniff(make({0x12, 0x00, 0x0A, 0x02, 0xAA, 0xBB}), "").format == Format::Obu);
  // Annex B: temporal_unit_size 5, frame_unit_size 4, obu_length 1 (temporal delimiter without size field),
  // then an OBU of length 2 (sequence header header byte 0x08 + 1 payload byte).
  CHECK(sniff(make({0x05, 0x04, 0x01, 0x10, 0x02, 0x08})).format == Format::Obu);
  // Neither: forbidden bit set.
  CHECK(sniff(make({0x92, 0x00, 0x0A, 0x02, 0xAA, 0xBB})).format == Format::Unknown);
}

TEST_CASE("sniff: raw only by extension", "[container][sniff][fast]") {
  const Bytes data(64, 0x80);
  CHECK(sniff(data, ".yuv").format == Format::Raw);
  CHECK(sniff(data, ".raw").format == Format::Raw);
  CHECK(sniff(data, ".rgb").format == Format::Raw);
  CHECK(sniff(data, ".bin").format == Format::Unknown);
  // Content wins over the extension.
  CHECK(sniff(make({'D', 'K', 'I', 'F'}, 32), ".yuv").format == Format::Ivf);
}

TEST_CASE("sniff: reads at most 64 KiB", "[container][sniff][fast]") {
  Bytes big(size_t{200} * 1024, 0x20);
  const Bytes key = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0D, 0x01, 0x02, 0x01};
  std::copy(key.begin(), key.end(), big.begin() + std::ptrdiff_t{100} * 1024);  // beyond the window
  CHECK(sniff(big).format == Format::Unknown);
}

TEST_CASE("sniff: empty and tiny inputs", "[container][sniff][fast]") {
  CHECK(sniff({}).format == Format::Unknown);
  CHECK(sniff(make({0x00})).format == Format::Unknown);
  CHECK(sniff(make({0x47})).format == Format::Unknown);
}

TEST_CASE("sniff: every container-smoke corpus entry", "[container][sniff][needs-corpus][fast]") {
  std::ifstream in(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/manifest.json");
  REQUIRE(in);
  const auto manifest = nlohmann::json::parse(in);
  for (const auto& entry : manifest.at("entries")) {
    const auto& tags = entry.value("tags", nlohmann::json::array());
    if (std::find(tags.begin(), tags.end(), "container-smoke") == tags.end()) {
      continue;
    }
    const std::string id = entry.at("id");
    DYNAMIC_SECTION(id) {
      const auto path = ovt::test::corpus_or_skip(id);
      const auto file = ovt::io::RandomAccessFile::open(path.string());
      // The manifest spells the EBML family "matroska"; ContainerTree.format says "ebml".
      // SPEC-ASSUMPTION(#138): the test maps the manifest name to the schema name.
      auto expected = entry.at("expected").at("format").get<std::string>();
      if (expected == "matroska") {
        expected = "ebml";
      }
      CHECK(std::string(ovt::container::format_name(ovt::container::sniff(*file))) == expected);
    }
  }
}

TEST_CASE("ovt_container_open on a 7-byte file returns OVT_E_UNSUPPORTED", "[container][open][fast]") {
  const TempFile f("seven.bin", Bytes(7, 0x5A));
  ovt_File handle{ovt::io::RandomAccessFile::open(f.path())};
  auto* c = reinterpret_cast<ovt_Container*>(0x1);
  CHECK(ovt_container_open(&handle, &c) == OVT_E_UNSUPPORTED);
  CHECK(c == nullptr);
}

TEST_CASE("ovt_container_open: stub reader, tree and close", "[container][open][fast]") {
  // ISOBMFF keeps the stub reader until WI-1.201; the IVF reader has its own tests (ivf_test.cpp).
  const TempFile f("stub.mp4", box(32, "ftyp", 64));
  ovt_File handle{ovt::io::RandomAccessFile::open(f.path())};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  REQUIRE(c != nullptr);
  CHECK(std::string(ovt_container_format(c)) == "isobmff");
  CHECK(ovt_container_size(c) == 64);
  CHECK(ovt_container_track_count(c) == 0);
  const ovt_Buffer tree = ovt_container_tree(c);
  CHECK(tree.data != nullptr);
  CHECK(tree.size > 0);
  CHECK(ovt_container_expand(c, 0) == OVT_E_UNSUPPORTED);
  ovt_Buffer out{nullptr, 0};
  CHECK(ovt_container_bytes(c, 0, 16, &out) == OVT_E_UNSUPPORTED);
  ovt_container_close(c);
  ovt_container_close(nullptr);  // no-op
}

TEST_CASE("ovt_container_open_format argument rules", "[container][open][fast]") {
  const TempFile f("any.bin", Bytes(64, 0x80));
  ovt_File handle{ovt::io::RandomAccessFile::open(f.path())};
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open_format(&handle, "nonsense", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_open_format(&handle, "raw", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_open_format(&handle, "mkv", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_open_format(&handle, "ebml", nullptr, nullptr, &c) == OVT_E_UNSUPPORTED);
  CHECK(ovt_container_open_format(nullptr, "isobmff", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  REQUIRE(ovt_container_open_format(&handle, "raw", "16x16:yuv420p", nullptr, &c) == OVT_OK);
  CHECK(std::string(ovt_container_format(c)) == "raw");
  ovt_container_close(c);
}

TEST_CASE("CNT diagnostic registry", "[container][diag][fast]") {
  using ovt::container::DiagCode;
  const auto all = ovt::container::all_diagnostics();
  REQUIRE(!all.empty());
  for (size_t i = 0; i < all.size(); ++i) {
    for (size_t j = i + 1; j < all.size(); ++j) {
      CHECK(all[i].code != all[j].code);  // a code appears exactly once
    }
  }
  const auto& d = ovt::container::diag(DiagCode::CNT_0006);
  CHECK(d.code == "CNT-0006");
  CHECK(d.severity == ovt::container::Severity::Fatal);
  CHECK(ovt::container::diag("CNT-0007") != nullptr);
  CHECK(ovt::container::diag("CNT-9999") == nullptr);
}
