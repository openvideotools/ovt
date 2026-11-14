// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MXF placeholder reader (chapter 08 §8.6.7, WI-4.211): the libavformat-backed tree (format "mxf", one root
// node with the "not implemented" description, TrackInfo per stream) and Role B packets on the same file.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "container/format.hpp"
#include "container/mxf_placeholder.hpp"
#include "demux/demuxer.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace mxf = ovt::container::mxf;

// A header partition pack key (SMPTE 377-1 §7.1) followed by bytes no MXF demuxer accepts: the sniffer says
// "mxf", libavformat does not.
Bytes fake_mxf() {
  Bytes b = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0D, 0x01, 0x02, 0x01, 0x01, 0x02, 0x04, 0x00};
  b.resize(4096, 0xFF);
  return b;
}

Bytes copy_of(const ovt_Buffer& b) {
  const std::span<const uint8_t> bytes(static_cast<const uint8_t*>(b.data), b.size);
  return {bytes.begin(), bytes.end()};
}

}  // namespace

TEST_CASE("mxf placeholder: bytes libavformat rejects fail the open with its status", "[container][mxf][fast]") {
  const Bytes data = fake_mxf();
  ovt::io::MemoryFile file{std::span<const uint8_t>(data)};
  REQUIRE(ovt::container::sniff(file) == ovt::container::Format::Mxf);
  CHECK(ovt::container::has_reader(ovt::container::Format::Mxf));

  ovt_status why = OVT_OK;
  CHECK(mxf::make_reader(file, &why) == nullptr);
  CHECK((why == OVT_E_CORRUPT || why == OVT_E_UNSUPPORTED));
  CHECK(mxf::make_reader(file) == nullptr);  // `why` is optional

  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(std::span<const uint8_t>(data))};
  ovt_Container* c = nullptr;
  const ovt_status st = ovt_container_open(&handle, &c);
  CHECK((st == OVT_E_CORRUPT || st == OVT_E_UNSUPPORTED));
  CHECK(c == nullptr);
  const ovt_status forced = ovt_container_open_format(&handle, "mxf", nullptr, nullptr, &c);
  CHECK((forced == OVT_E_CORRUPT || forced == OVT_E_UNSUPPORTED));
  CHECK(c == nullptr);
}

TEST_CASE("mxf placeholder: an empty file forced to mxf is not opened", "[container][mxf][fast]") {
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(std::span<const uint8_t>())};
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open_format(&handle, "mxf", nullptr, nullptr, &c) != OVT_OK);
  CHECK(c == nullptr);
}

// WI-4.211 acceptance test 1. The corpus is the OP1a MPEG-2 422 file of recipe 77
// (SPEC-ASSUMPTION(#429): the spec's tests/corpora/mxf/op1a_xdcam.mxf has no source; the synthetic recipe
// syn_mxf_op1a_xdcam provides it, with the alias op1a_xdcam.mxf).
TEST_CASE("mxf placeholder: OP1a XDCAM file probes as mxf with one MPEG-2 video track and readable packets",
          "[container][mxf][needs-corpus][fast]") {
  const auto path = ovt::test::corpus_or_skip("syn_mxf_op1a_xdcam");
  auto owned = ovt::io::RandomAccessFile::open(path.string());
  REQUIRE(owned);
  const uint64_t size = owned->size();
  ovt_File handle{std::move(owned)};

  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  REQUIRE(c != nullptr);
  CHECK(std::string(ovt_container_format(c)) == "mxf");

  const ovt_Buffer buf = ovt_container_tree(c);
  REQUIRE(buf.data != nullptr);
  flatbuffers::Verifier verifier(static_cast<const uint8_t*>(buf.data), buf.size);
  REQUIRE(verifier.VerifyBuffer<ovt::fb::ContainerTree>(nullptr));
  const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data);
  REQUIRE(tree->format() != nullptr);
  CHECK(tree->format()->str() == "mxf");
  REQUIRE(tree->root() != nullptr);
  REQUIRE(tree->root()->size() == 1);
  const auto* root = tree->root()->Get(0);
  REQUIRE(root->description() != nullptr);
  CHECK(root->description()->str() == std::string(mxf::kPlaceholderDescription));
  REQUIRE(root->file_span() != nullptr);
  CHECK(root->file_span()->offset() == 0);
  CHECK(root->file_span()->length() == size);

  REQUIRE(tree->tracks() != nullptr);
  CHECK(ovt_container_track_count(c) == static_cast<int>(tree->tracks()->size()));
  int video_tracks = 0;
  int video_avstream = -1;
  for (const auto* t : *tree->tracks()) {
    if (t->handler() == ovt::fb::HandlerType::Video) {
      ++video_tracks;
      video_avstream = t->avstream_index();
      CHECK(t->codec() == ovt::fb::Codec::MPEG2);
      CHECK(t->coded_width() == 1920);
      CHECK(t->coded_height() == 1080);
      CHECK(t->timescale() > 0);
      CHECK(t->duration_seconds() > 0.0);
    }
  }
  CHECK(video_tracks == 1);
  REQUIRE(video_avstream >= 0);
  const Bytes first = copy_of(buf);
  ovt_container_close(c);

  // Determinism (§0.7.5): a second open serialises the same bytes.
  ovt_Container* again = nullptr;
  REQUIRE(ovt_container_open(&handle, &again) == OVT_OK);
  const ovt_Buffer buf2 = ovt_container_tree(again);
  CHECK(copy_of(buf2) == first);
  ovt_container_close(again);

  // Playback packets (Role B) of the same file.
  auto opened = ovt::demux::Demuxer::open(ovt::io::RandomAccessFile::open(path.string()), "mxf");
  REQUIRE(opened.has_value());
  auto& demuxer = *opened.value();
  const std::vector<int> only_video = {video_avstream};
  demuxer.select_streams(only_video);
  int packets = 0;
  bool first_is_key = false;
  for (;;) {
    auto p = demuxer.read_packet();
    REQUIRE(p.has_value());
    if (!p.value()) {
      break;
    }
    CHECK(p.value()->avstream_index == video_avstream);
    CHECK_FALSE(p.value()->data.empty());
    if (packets == 0) {
      first_is_key = p.value()->keyframe;
    }
    ++packets;
  }
  CHECK(packets == 12);
  CHECK(first_is_key);
}
