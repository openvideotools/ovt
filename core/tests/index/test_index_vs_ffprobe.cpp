// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 and H.265 tests of the StreamIndex builder (chapter 7 §7.4.2, WI-1.121 acceptance test 1): the frames of
// an Annex B stream against `ffprobe -show_frames`. ffprobe lists decoded frames in output order with the file
// position and size of the packet (access unit) each came from, which pins down three things at once: which units
// the builder groups into a frame, the frame's type and its place in the display order. An Annex B stream has no
// container timing and ffprobe prints no pts for it, so "pts match" is the display position here.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "container/units_oracle.hpp"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

namespace fb = ovt::fb;
namespace idx = ovt::index;
using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Bytes index_annexb(std::span<const uint8_t> stream, fb::Codec codec) {
  idx::StreamIndexBuilder b(idx::BuilderOptions{codec});
  ovt::syntax::scan_annexb(
      stream, 0, ovt::syntax::kNoContainerSample,
      [&b](const ovt::syntax::ScannedUnit& u) {
        idx::UnitInput in;
        in.file_offset = u.file_offset;
        in.bytes = u.bytes;
        in.prefix_span = u.prefix_span;
        in.sample_index = u.container_sample_index;
        b.add_unit(in);
      },
      [&b](const ovt::syntax::ScanDiagnostic& d) { b.add_scan_diagnostic(d); });
  const std::array<uint8_t, 32> hash{};
  Bytes index = b.finish(hash);
  for (const auto& issue : ovt::model::validate_stream_index(index)) {
    FAIL_CHECK(issue.rule << ": " << issue.message);
  }
  return index;
}

const fb::StreamIndex& root(const Bytes& index) {
  return *flatbuffers::GetRoot<fb::StreamIndex>(index.data());
}

char type_letter(fb::FrameType t) {
  switch (t) {
    case fb::FrameType::I:
      return 'I';
    case fb::FrameType::P:
      return 'P';
    case fb::FrameType::B:
      return 'B';
    default:
      return '?';
  }
}

// The frame types in display order, e.g. "IBBP".
std::string display_types(const fb::StreamIndex& si) {
  std::string out;
  for (const int32_t d : *si.display_order()) {
    out += type_letter(si.frames()->Get(static_cast<uint32_t>(d))->type());
  }
  return out;
}

// File offset of the first byte of a frame (the prefix of its first unit).
uint64_t frame_start(const fb::StreamIndex& si, const fb::FrameInfo& f) {
  const fb::Unit* u = si.units()->Get(f.units()->Get(0));
  return u->file_span()->offset() - u->prefix_span()->length();
}

// Bytes of the end of sequence / end of bitstream NAL units (H.265 types 36, 37) that close a frame. FFmpeg's hevc
// parser starts its next packet at such a unit, while the index keeps it in the access unit it ends (§7.3.9).
uint64_t trailing_eos_bytes(const fb::StreamIndex& si, const fb::FrameInfo& f) {
  uint64_t bytes = 0;
  if (si.codec() != fb::Codec::H265) {
    return bytes;
  }
  for (uint32_t i = f.units()->size(); i > 0; --i) {
    const fb::Unit* u = si.units()->Get(f.units()->Get(i - 1));
    if (u->type() != 36 && u->type() != 37) {
      break;
    }
    bytes += u->file_span()->length() + u->prefix_span()->length();
  }
  return bytes;
}

// Where FFmpeg starts the packet of a frame: at the frame, or at the EOS / EOB units that end the frame before it.
uint64_t packet_start(const fb::StreamIndex& si, const fb::FrameInfo& f) {
  const int32_t d = f.id()->decode_index();
  const uint64_t moved = d > 0 ? trailing_eos_bytes(si, *si.frames()->Get(static_cast<uint32_t>(d - 1))) : 0;
  return frame_start(si, f) - moved;
}

struct ProbeFrame {
  uint64_t pos = 0;
  uint64_t size = 0;
  char type = '?';
};

// `ffprobe -show_frames` of a raw stream, in output order; nullopt when ffprobe is missing or decodes nothing.
std::optional<std::vector<ProbeFrame>> ffprobe_frames(const std::filesystem::path& path, std::string_view format) {
  if (!ovt::test::units::tool_on_path("ffprobe")) {
    return std::nullopt;
  }
  const auto out = ovt::test::units::capture("ffprobe -v error -f " + std::string(format) +
                                             " -show_frames -show_entries frame=pkt_pos,pkt_size,pict_type"
                                             " -of default=nw=1 '" +
                                             path.string() + "' 2>/dev/null");
  if (!out) {
    return std::nullopt;
  }
  std::vector<ProbeFrame> frames;
  std::istringstream lines(*out);
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("pkt_pos=")) {
      frames.push_back({std::stoull(line.substr(8)), 0, '?'});
    } else if (line.starts_with("pkt_size=") && !frames.empty()) {
      frames.back().size = std::stoull(line.substr(9));
    } else if (line.starts_with("pict_type=") && line.size() > 10 && !frames.empty()) {
      frames.back().type = line[10];
    }
  }
  return frames.empty() ? std::nullopt : std::optional(std::move(frames));
}

struct Compare {
  bool types = true;       // H.265: FFmpeg's pict_type follows the first slice, so only single-slice streams compare
  bool all_output = true;  // false: FFmpeg drops pictures the index keeps (RASL at the start, no_output_of_prior_pics)
  // FFmpeg's hevc parser cuts packets at the 3-byte start code, so the zero_byte of a 4-byte start code ends the
  // previous packet; the index counts it to the access unit it precedes (§7.3.3). Its h264 parser moves that byte to
  // the next packet, as the index does.
  uint64_t slack = 0;
};

bool near(uint64_t a, uint64_t b, uint64_t slack) {
  return (a > b ? a - b : b - a) <= slack;
}

// Every ffprobe frame is the index frame that starts at its packet position, with that packet's size and picture
// type, and the frames come in the index's display order.
void compare_with_ffprobe(const fb::StreamIndex& si, const std::vector<ProbeFrame>& probe, const Compare& how) {
  std::vector<const fb::FrameInfo*> shown;  // display order; the first field stands for a field pair
  for (const int32_t d : *si.display_order()) {
    shown.push_back(si.frames()->Get(static_cast<uint32_t>(d)));
  }
  if (how.all_output) {
    REQUIRE(shown.size() == probe.size());
  }
  size_t next = 0;
  for (size_t k = 0; k < probe.size(); ++k) {
    CAPTURE(k, probe[k].pos);
    while (next < shown.size() && !near(packet_start(si, *shown[next]), probe[k].pos, how.slack)) {
      REQUIRE_FALSE(how.all_output);  // the index's display order has a frame here that ffprobe did not output
      ++next;
    }
    REQUIRE(next < shown.size());
    const fb::FrameInfo& f = *shown[next++];
    if (how.types) {
      CHECK(type_letter(f.type()) == probe[k].type);
    }
    // A complementary field pair is one FFmpeg packet and two FrameInfo that share the display index.
    uint64_t size = f.size_bytes();
    const fb::FrameInfo* last = &f;
    const auto next_decode = static_cast<uint32_t>(f.id()->decode_index()) + 1;
    if (f.structure() != fb::PictureStructure::Frame && f.structure() != fb::PictureStructure::Mbaff &&
        next_decode < si.frames()->size() &&
        si.frames()->Get(next_decode)->id()->display_index() == f.id()->display_index()) {
      last = si.frames()->Get(next_decode);
      size += last->size_bytes();
    }
    CHECK(near(frame_start(si, f) + size - trailing_eos_bytes(si, *last), probe[k].pos + probe[k].size, how.slack));
  }
}

// A committed seed of the index_builder fuzz target without its selector byte.
Bytes seed(std::string_view name) {
  Bytes b = read_file(std::filesystem::path(OVT_INDEX_BUILDER_SEEDS) / name);
  REQUIRE(b.size() > 1);
  b.erase(b.begin());
  return b;
}

}  // namespace

// libx264, 12 frames of 16x16, keyint 6, two B-frames, an AUD per access unit. Expected values: ffprobe 6.1.
TEST_CASE("index_builder_h264: IDR period with B-frames", "[index][h264][fast]") {
  const Bytes stream = seed("seed_2_h264_bframes.bin");
  const Bytes index = index_annexb(stream, fb::Codec::H264);
  CHECK(index == index_annexb(stream, fb::Codec::H264));  // §0.7.5
  const fb::StreamIndex& si = root(index);
  REQUIRE(si.frames()->size() == 12);
  CHECK(display_types(si) == "IBBPBPIBBPBP");
  const std::array<int32_t, 12> order = {0, 2, 3, 1, 5, 4, 6, 8, 9, 7, 11, 10};
  for (size_t k = 0; k < order.size(); ++k) {
    CHECK(si.display_order()->Get(static_cast<uint32_t>(k)) == order.at(k));
  }
  uint64_t total = 0;
  for (const fb::FrameInfo* f : *si.frames()) {
    total += f->size_bytes();
    CHECK(f->is_rap() == (f->id()->decode_index() % 6 == 0));
    CHECK(f->structure() == fb::PictureStructure::Frame);
    CHECK(f->coded_size()->w() == 16);
    CHECK(f->crop()->h() == 16);
    CHECK(f->slice_count() == 1);
    // x264 writes fixed-frame-rate VUI timing: 25 fps is two ticks of 1/50 s per frame (chapter 6 §6.2.6).
    CHECK(f->timebase_num() == 1);
    CHECK(f->timebase_den() == 50);
    CHECK(f->pts() == int64_t{f->id()->display_index()} * 2);
    CHECK(f->dts() == int64_t{f->id()->decode_index()} * 2);
  }
  CHECK(total == stream.size());  // every unit (AUD, SPS, PPS, slices) belongs to a frame
  CHECK(si.frames()->Get(0)->units()->size() == 4);
  CHECK(si.frames()->Get(1)->poc() > si.frames()->Get(2)->poc());
  CHECK(si.parameter_sets()->size() == 4);
  CHECK(si.diagnostics() == nullptr);
}

// libx265, 12 frames of 16x16, keyint 6, open GOP: the second intra picture is a CRA whose leading pictures
// precede it in display order.
TEST_CASE("index_builder_h265: open GOP with leading pictures", "[index][h265][fast]") {
  const Bytes stream = seed("seed_3_h265_open_gop.bin");
  const Bytes index = index_annexb(stream, fb::Codec::H265);
  CHECK(index == index_annexb(stream, fb::Codec::H265));
  const fb::StreamIndex& si = root(index);
  REQUIRE(si.frames()->size() == 12);
  CHECK(display_types(si) == "IBBPBBIBBPBP");
  uint64_t total = 0;
  size_t raps = 0;
  for (const fb::FrameInfo* f : *si.frames()) {
    total += f->size_bytes();
    raps += f->is_rap() ? 1U : 0U;
    CHECK(f->shown());
    CHECK(f->coded_size()->h() == 16);
    CHECK(f->timebase_den() == 25);
    CHECK(f->pts() == f->id()->display_index());
  }
  CHECK(total == stream.size());
  CHECK(raps == 2);
  CHECK(si.frames()->Get(4)->is_rap());                    // the CRA is the fifth picture in decode order
  CHECK(si.frames()->Get(4)->id()->display_index() == 6);  // and the seventh in display order
  CHECK(si.frames()->Get(4)->poc() > si.frames()->Get(5)->poc());
  CHECK(si.diagnostics() == nullptr);
}

TEST_CASE("index_builder_h26x: slices without parameter sets become frames of type Unknown",
          "[index][h264][h265][fast]") {
  struct Case {
    fb::Codec codec;
    const char* seed;
    uint8_t max_vcl_type;  // the VCL units are nal_unit_type 1..this
    uint32_t unknown;      // pictures before the encoder repeats its parameter sets (at the next IDR / CRA)
  };
  const std::array<Case, 2> cases = {
      {{fb::Codec::H264, "seed_2_h264_bframes.bin", 5, 6}, {fb::Codec::H265, "seed_3_h265_open_gop.bin", 31, 4}}};
  for (const Case& c : cases) {
    CAPTURE(c.seed);
    const Bytes whole = seed(c.seed);
    // Cut the stream at its first slice: the parameter sets before it are gone.
    uint64_t cut = 0;
    const Bytes ref_index = index_annexb(whole, c.codec);
    for (const fb::Unit* u : *root(ref_index).units()) {
      if (u->type() >= 1 && u->type() <= c.max_vcl_type) {
        cut = u->file_span()->offset() - u->prefix_span()->length();
        break;
      }
    }
    REQUIRE(cut > 0);
    const Bytes index = index_annexb(std::span<const uint8_t>(whole).subspan(cut), c.codec);
    const fb::StreamIndex& si = root(index);
    REQUIRE(si.frames()->size() == 12);
    for (uint32_t i = 0; i < 12; ++i) {
      CAPTURE(i);
      CHECK((si.frames()->Get(i)->type() == fb::FrameType::Unknown) == (i < c.unknown));
    }
    size_t warned = 0;
    REQUIRE(si.diagnostics() != nullptr);
    for (const fb::Diagnostic* d : *si.diagnostics()) {
      warned += d->code()->string_view() == "SYN-GEN-0102" ? 1U : 0U;
    }
    CHECK(warned == 1);
  }
}

namespace {

// The stream again with an end of sequence NAL unit after the first coded video sequence. FFmpeg then starts the
// next packet at that unit and drops the pictures it had not output yet, and the leading pictures after it.
void compare_with_end_of_sequence(const Bytes& stream, const std::filesystem::path& original) {
  const Bytes plain_index = index_annexb(stream, fb::Codec::H265);
  const fb::StreamIndex& plain = root(plain_index);
  uint64_t cut = 0;  // start of the second random access picture's access unit
  for (const fb::FrameInfo* f : *plain.frames()) {
    if (f->is_rap() && f->id()->decode_index() > 0) {
      cut = frame_start(plain, *f);
      break;
    }
  }
  REQUIRE(cut > 0);
  Bytes with_eos(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(cut));
  with_eos.insert(with_eos.end(), {0x00, 0x00, 0x01, 0x4A, 0x01});  // EOS_NUT, nuh_temporal_id_plus1 = 1
  with_eos.insert(with_eos.end(), stream.begin() + static_cast<std::ptrdiff_t>(cut), stream.end());
  std::filesystem::path path = original;
  path += ".eos.265";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): std::ostream::write takes char*
    out.write(reinterpret_cast<const char*>(with_eos.data()), static_cast<std::streamsize>(with_eos.size()));
  }
  const auto probe = ffprobe_frames(path, "hevc");
  REQUIRE(probe);
  const Bytes index = index_annexb(with_eos, fb::Codec::H265);
  CHECK(root(index).frames()->size() == plain.frames()->size());
  compare_with_ffprobe(root(index), *probe, Compare{true, false, 1});
}

}  // namespace

// AT1 on streams FFmpeg encodes at test time (skipped without ffmpeg / ffprobe).
TEST_CASE("index_vs_ffprobe: FFmpeg-encoded H.264 and H.265 streams", "[index][h264][h265][ffprobe][fast]") {
  using ovt::test::units::MadeStream;
  struct Case {
    fb::Codec codec;
    const char* format;
    const char* file;
    const char* encode;
  };
  const std::array<Case, 5> cases = {{
      {fb::Codec::H264, "h264", "index_a.264", "-frames:v 24 -c:v libx264 -x264-params keyint=8:bframes=2:slices=2"},
      {fb::Codec::H264, "h264", "index_b.264",
       "-frames:v 30 -c:v libx264 -x264-params keyint=12:bframes=3:b-pyramid=normal:ref=3:open-gop=1"},
      {fb::Codec::H264, "h264", "index_c.264",
       "-frames:v 20 -c:v libx264 -flags +ildct+ilme -x264-params keyint=10:bframes=1:tff=1"},
      {fb::Codec::H265, "hevc", "index_a.265",
       "-frames:v 24 -c:v libx265 -x265-params keyint=8:bframes=2:log-level=none"},
      {fb::Codec::H265, "hevc", "index_b.265",
       "-frames:v 30 -c:v libx265 -x265-params keyint=10:bframes=3:b-pyramid=1:open-gop=1:log-level=none"},
  }};
  size_t compared = 0;
  for (const Case& c : cases) {
    {
      CAPTURE(c.file);
      const MadeStream stream(c.file, ovt::test::units::kTestSource, c.encode);
      if (!stream.ok()) {
        continue;
      }
      const auto probe = ffprobe_frames(stream.path(), c.format);
      if (!probe) {
        continue;
      }
      const Bytes bytes = read_file(stream.path());
      const Bytes index = index_annexb(bytes, c.codec);
      compare_with_ffprobe(root(index), *probe, Compare{true, true, c.codec == fb::Codec::H265 ? 1U : 0U});
      ++compared;
      if (c.codec == fb::Codec::H265) {
        compare_with_end_of_sequence(bytes, stream.path());
      }
    }
  }
  if (compared == 0) {
    SKIP("ffmpeg or ffprobe is not available");
  }
}

namespace {

struct CorpusCase {
  const char* id;
  fb::Codec codec;
  const char* format;
  Compare how;
};

// Compares every available stream of `cases` with ffprobe; skips when none could be compared.
void compare_corpus_streams(std::span<const CorpusCase> cases) {
  size_t compared = 0;
  for (const CorpusCase& c : cases) {
    CAPTURE(c.id);
    const std::optional<std::filesystem::path> path = ovt::test::corpus_path(c.id);
    if (!path) {
      if (ovt::test::require_corpora()) {
        FAIL("corpus file missing: " << c.id);
      }
      continue;
    }
    const auto probe = ffprobe_frames(*path, c.format);
    if (!probe) {
      continue;
    }
    const Bytes index = index_annexb(read_file(*path), c.codec);
    compare_with_ffprobe(root(index), *probe, c.how);
    ++compared;
  }
  if (compared == 0) {
    SKIP("the corpus or ffprobe is not available");
  }
}

}  // namespace

// AT1: Given ffprobe -show_frames of each test stream (WI-1.121 step 7), When compared with StreamIndex.frames
// (shown frames), Then counts, pict_type and the display position match for every frame.
TEST_CASE("index_vs_ffprobe: conformance streams", "[index][h264][h265][fast][needs-corpus][needs-oracle:ffprobe]") {
  const std::array<CorpusCase, 8> cases = {{
      {"jvt_BA1_Sony_D", fb::Codec::H264, "h264", {}},
      {"jvt_CVSE2_Sony_B", fb::Codec::H264, "h264", {}},
      {"jvt_MR1_BT_A", fb::Codec::H264, "h264", {}},
      // WPP_A has one slice of several dependent segments per picture; the others are single-slice streams.
      {"jctvc_WPP_A_ericsson_MAIN_2", fb::Codec::H265, "hevc", {true, true, 1}},
      {"jctvc_AMP_A_Samsung_7", fb::Codec::H265, "hevc", {true, true, 1}},
      // RAP_A starts with a CRA whose RASL pictures FFmpeg drops; NoOutPrior_A discards pictures with
      // no_output_of_prior_pics_flag. The index keeps both kinds (§7.3.10.2, WI-1.123).
      {"jctvc_RAP_A_docomo_6", fb::Codec::H265, "hevc", {true, false, 1}},
      {"jctvc_POC_A_Bossen_3", fb::Codec::H265, "hevc", {true, true, 1}},
      {"jctvc_NoOutPrior_A_Qualcomm_1", fb::Codec::H265, "hevc", {true, false, 1}},
  }};
  compare_corpus_streams(cases);
}

// The two H.264 streams of step 7 that the corpus manifest tags slow (field and MBAFF coding, high profile): the
// same comparison in the nightly lane (chapter 14 §14.2.5).
TEST_CASE("index_vs_ffprobe: slow conformance streams", "[index][h264][slow][needs-corpus][needs-oracle:ffprobe]") {
  const std::array<CorpusCase, 2> cases = {{
      {"jvt_CAPAMA3_Sand_F", fb::Codec::H264, "h264", {}},
      {"jvt_HPCV_BRCM_A", fb::Codec::H264, "h264", {}},
  }};
  compare_corpus_streams(cases);
}
