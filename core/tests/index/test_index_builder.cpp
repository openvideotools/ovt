// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the StreamIndex builder (chapter 7 §7.4.2, WI-1.121 part a): the codec-independent frame bookkeeping,
// the AV1 dispatch end to end and the display order. The corpus-free cases index the libaom-encoded 16x16 streams
// of core/tests/fuzz/corpus/index_builder (made for this test: an alt-ref GOP with hidden frames and
// show_existing_frame headers); the [needs-corpus] cases index the libaom test vectors of the WI.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/ivf.hpp"
#include "index/builder_internal.h"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

namespace fb = ovt::fb;
namespace idx = ovt::index;
using Bytes = std::vector<uint8_t>;

constexpr std::array<uint8_t, 32> kHash = {1, 2, 3};

Bytes read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A seed of the index_builder fuzz target without its selector byte: a Section 5 OBU stream.
Bytes seed(std::string_view name) {
  Bytes b = read_file(std::filesystem::path(OVT_INDEX_BUILDER_SEEDS) / name);
  REQUIRE(b.size() > 1);
  b.erase(b.begin());
  return b;
}

idx::UnitInput input_of(const ovt::syntax::ScannedUnit& u, const idx::SampleTiming* timing = nullptr) {
  idx::UnitInput in;
  in.file_offset = u.file_offset;
  in.bytes = u.bytes;
  in.prefix_span = u.prefix_span;
  in.sample_index = u.container_sample_index;
  in.timing = timing;
  return in;
}

// Indexes a Section 5 OBU stream (an .obu elementary stream: no container timing).
Bytes index_obu(std::span<const uint8_t> stream) {
  idx::StreamIndexBuilder b(idx::BuilderOptions{fb::Codec::AV1});
  ovt::syntax::scan_obu_low_overhead(
      stream, 0, ovt::syntax::kNoContainerSample, ovt::syntax::ObuSpanKind::Section5Stream,
      [&b](const ovt::syntax::ScannedUnit& u) { b.add_unit(input_of(u)); },
      [&b](const ovt::syntax::ScanDiagnostic& d) { b.add_scan_diagnostic(d); });
  return b.finish(kHash);
}

// Indexes an IVF file: one sample per IVF frame, pts = dts = the IVF timestamp in scale / rate seconds (chapter 6
// §6.2.6). `cut` drops that many bytes from the end of the file first.
Bytes index_ivf(const std::filesystem::path& path, size_t cut = 0) {
  Bytes data = read_file(path);
  REQUIRE(data.size() > cut);
  data.resize(data.size() - cut);
  const auto tmp = std::filesystem::temp_directory_path() / ("ovt_index_builder_" + path.filename().string());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): std::ostream::write takes char*
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }
  const auto file = ovt::io::RandomAccessFile::open(tmp.string());
  const auto ivf = ovt::container::ivf::parse(*file);
  REQUIRE(ivf.has_value());
  idx::StreamIndexBuilder b(idx::BuilderOptions{fb::Codec::AV1});
  for (size_t i = 0; i < ivf->frames.size(); ++i) {
    const auto& fr = ivf->frames[i];
    const idx::SampleTiming timing{static_cast<int64_t>(fr.timestamp), static_cast<int64_t>(fr.timestamp),
                                   static_cast<int32_t>(ivf->header.timebase_numerator),
                                   static_cast<int32_t>(ivf->header.timebase_denominator)};
    const std::span<const uint8_t> payload(data.data() + fr.payload_offset(), fr.size);
    if (fr.size < fr.declared_size) {
      b.add_scan_diagnostic({"IO-0001",
                             "unexpected end of file at offset " + std::to_string(data.size()) + " (" +
                                 std::to_string(fr.declared_size) + " bytes requested)",
                             fr.header_offset});
    }
    ovt::syntax::scan_obu_low_overhead(
        payload, fr.payload_offset(), static_cast<int64_t>(i), ovt::syntax::ObuSpanKind::LengthDelimited,
        [&b, &timing](const ovt::syntax::ScannedUnit& u) { b.add_unit(input_of(u, &timing)); },
        [&b](const ovt::syntax::ScanDiagnostic& d) { b.add_scan_diagnostic(d); });
  }
  std::filesystem::remove(tmp);
  return b.finish(kHash);
}

const fb::StreamIndex& root(const Bytes& index) {
  return *flatbuffers::GetRoot<fb::StreamIndex>(index.data());
}

std::vector<std::string> codes(const fb::StreamIndex& si) {
  std::vector<std::string> out;
  if (si.diagnostics() != nullptr) {
    for (const auto* d : *si.diagnostics()) {
      out.push_back(d->code()->str());
    }
  }
  return out;
}

bool has_code(const fb::StreamIndex& si, std::string_view code) {
  return std::ranges::any_of(codes(si), [code](const std::string& c) { return c == code; });
}

// The V-001 / V-029 / V-031 rules of chapter 6 §6.9 hold for every index the builder writes.
void check_valid(const Bytes& index) {
  for (const auto& v : ovt::model::validate_stream_index(index)) {
    FAIL_CHECK(v.rule << ": " << v.message << " at " << v.path);
  }
}

// "K0 i1h P2 ..." — type letter (K key, P inter, I intra-only, S switch), decode index, 'h' for a hidden frame,
// "=N" for a show_existing_frame of decode index N.
std::string frame_tokens(const fb::StreamIndex& si) {
  std::string out;
  for (const auto* f : *si.frames()) {
    const char t = f->type() == fb::FrameType::Key         ? 'K'
                   : f->type() == fb::FrameType::Inter     ? 'P'
                   : f->type() == fb::FrameType::IntraOnly ? 'I'
                   : f->type() == fb::FrameType::Switch    ? 'S'
                                                           : '?';
    out += out.empty() ? "" : " ";
    out += t + std::to_string(f->id()->decode_index());
    if (!f->shown()) {
      out += 'h';
    }
    if (f->show_existing_of() >= 0) {
      out += '=' + std::to_string(f->show_existing_of());
    }
  }
  return out;
}

size_t count_shown(const fb::StreamIndex& si) {
  size_t n = 0;
  for (const auto* f : *si.frames()) {
    n += f->shown() ? 1U : 0U;
  }
  return n;
}

}  // namespace

// ---- BuilderCore: S5 / S7 bookkeeping ---------------------------------------------------------------------------

TEST_CASE("index_builder_core: pending units join the next frame and sizes include prefixes", "[index][fast]") {
  idx::BuilderCore core;
  const auto add = [&core](uint64_t length, uint64_t prefix) {
    idx::UnitRec u;
    u.length = length;
    u.prefix_span.length = prefix;
    core.units.push_back(std::move(u));
    return static_cast<uint32_t>(core.units.size() - 1);
  };
  core.add_pending(add(10, 4));  // parameter set
  core.add_pending(add(5, 3));   // SEI
  idx::FrameRec& f0 = core.start_frame(add(100, 3));
  CHECK(f0.decode_index == 0);
  CHECK(f0.units == std::vector<uint32_t>{0, 1, 2});
  core.extend_frame(add(50, 3));
  core.add_pending(add(2, 4));         // AUD of the next access unit
  (void)core.start_frame(add(70, 3));  // finishes frame 0
  REQUIRE(core.frames.size() == 1);
  CHECK(core.frames[0].size_bytes == 10 + 4 + 5 + 3 + 100 + 3 + 50 + 3);
  CHECK(core.units[3].frame == 0);
  CHECK(core.units[4].frame == 1);
  core.add_pending(add(1, 3));  // trailing non-VCL unit: no frame owns it
  core.finish_frame();
  core.finish_frame();  // no-op
  REQUIRE(core.frames.size() == 2);
  CHECK(core.frames[1].units == std::vector<uint32_t>{4, 5});
  CHECK(core.frames[1].size_bytes == 2 + 4 + 70 + 3);
  CHECK(core.units[6].frame == -1);

  core.unit_diagnostic(5, idx::DiagRec{ovt::syntax::Severity::Error, "BR-0001", "truncated", {8, 0}});
  REQUIRE(core.diagnostics.size() == 1);
  CHECK(core.diagnostics[0].message == "unit 5: truncated");
  CHECK(core.units[5].diagnostics.size() == 1);
}

// ---- S9 ---------------------------------------------------------------------------------------------------------

TEST_CASE("index_display_order: POC order within a sequence, fields share a slot", "[index][fast]") {
  const auto frame = [](int32_t di, int64_t poc, uint32_t sequence, bool shown = true, int32_t second_of = -1) {
    idx::FrameRec f;
    f.decode_index = di;
    f.poc = poc;
    f.sequence = sequence;
    f.shown = shown;
    f.second_field_of = second_of;
    return f;
  };
  // Sequence 0: I0 P8 B4 B2(not output) B6; sequence 1: a field pair (POC 0, 1) then a pair at POC 4, 5 and a
  // pair at POC 2, 3.
  std::vector<idx::FrameRec> frames = {frame(0, 0, 0),          frame(1, 8, 0),          frame(2, 4, 0),
                                       frame(3, 2, 0, false),   frame(4, 6, 0),          frame(5, 0, 1),
                                       frame(6, 1, 1, true, 5), frame(7, 4, 1),          frame(8, 5, 1, true, 7),
                                       frame(9, 2, 1),          frame(10, 3, 1, true, 9)};
  const std::vector<int32_t> order = idx::assign_display_order(fb::Codec::H264, frames);
  CHECK(order == std::vector<int32_t>{0, 2, 4, 1, 5, 9, 7});
  const std::vector<int32_t> expect = {0, 3, 1, -1, 2, 4, 4, 6, 6, 5, 5};
  for (size_t i = 0; i < frames.size(); ++i) {
    CAPTURE(i);
    CHECK(frames[i].display_index == expect[i]);
  }

  // AV1: decode order of the shown frames; a hidden frame keeps -1.
  std::vector<idx::FrameRec> av1 = {frame(0, 0, 0), frame(1, 4, 0, false), frame(2, 1, 0), frame(3, 4, 0)};
  CHECK(idx::assign_display_order(fb::Codec::AV1, av1) == std::vector<int32_t>{0, 2, 3});
  CHECK(av1[1].display_index == -1);
  CHECK(av1[3].display_index == 2);
}

// ---- AV1 end to end, no corpus ----------------------------------------------------------------------------------

TEST_CASE("index_builder_av1: alt-ref GOP with hidden frames and show_existing_frame", "[index][av1][fast]") {
  const Bytes stream = seed("seed_0_altref_gop.bin");
  const Bytes index = index_obu(stream);
  check_valid(index);
  const fb::StreamIndex& si = root(index);
  CHECK(si.codec() == fb::Codec::AV1);
  CHECK(si.schema_version() == 1);
  REQUIRE(si.source_hash()->size() == 32);
  CHECK(si.source_hash()->Get(2) == 3);

  // libaom 3.8 (ffmpeg -c:v libaom-av1 -lag-in-frames 8 -auto-alt-ref 1, 7 input frames of 16x16): frames 1 and 2
  // are hidden and shown later through show_existing_frame headers (frames 8 and 5). ffprobe -show_packets counts 7
  // temporal units, i.e. 7 shown frames.
  CHECK(frame_tokens(si) == "K0 P1h P2h P3 P4 P5=2 P6 P7 P8=1");
  CHECK(count_shown(si) == 7);
  CHECK(si.frames()->size() == count_shown(si) + 2);  // AT2: shown + hidden
  REQUIRE(si.display_order()->size() == count_shown(si));
  const std::vector<int32_t> order(si.display_order()->begin(), si.display_order()->end());
  CHECK(order == std::vector<int32_t>{0, 3, 4, 5, 6, 7, 8});
  CHECK(si.frames()->Get(1)->id()->display_index() == -1);
  CHECK(si.frames()->Get(8)->id()->display_index() == 6);

  const fb::FrameInfo& key = *si.frames()->Get(0);
  CHECK(key.is_rap());
  CHECK(key.structure() == fb::PictureStructure::Frame);
  CHECK(key.coded_size()->w() == 16);
  CHECK(key.coded_size()->h() == 16);
  CHECK(key.crop()->w() == 16);
  CHECK(key.chroma_format() == 1);
  CHECK(key.bit_depth_luma() == 8);
  CHECK(key.slice_count() == 1);
  CHECK(key.frame_num() == -1);
  CHECK(key.declared_refs() == nullptr);  // DPB models are stubs until WI-1.124
  CHECK(key.dpb_after() == nullptr);
  // units of the key frame: temporal delimiter, sequence header, frame.
  REQUIRE(key.units()->size() == 3);
  CHECK(si.units()->Get(0)->type_name()->string_view() == "OBU_TEMPORAL_DELIMITER");
  CHECK(si.units()->Get(1)->type_name()->string_view() == "OBU_SEQUENCE_HEADER");
  CHECK(si.units()->Get(2)->type_name()->string_view() == "OBU_FRAME");
  CHECK(si.units()->Get(2)->header()->name()->string_view() == "open_bitstream_unit");
  CHECK(si.units()->Get(0)->frame()->decode_index() == 0);
  REQUIRE(si.parameter_sets()->size() == 1);
  CHECK(si.parameter_sets()->Get(0)->index() == 1);

  const fb::FrameInfo& hidden = *si.frames()->Get(1);
  const fb::FrameInfo& existing = *si.frames()->Get(8);
  CHECK_FALSE(hidden.is_rap());
  CHECK_FALSE(existing.is_rap());
  CHECK(existing.poc() == hidden.poc());  // RefOrderHint of the shown slot
  CHECK(existing.qp_base() == hidden.qp_base());
  CHECK(existing.coded_size()->w() == 16);
  CHECK(existing.slice_count() == 0);
  // the show_existing entry is its frame header OBU plus the temporal delimiter before it
  REQUIRE(existing.units()->size() == 2);
  CHECK(si.units()->Get(existing.units()->Get(1))->type_name()->string_view() == "OBU_FRAME_HEADER");

  // S7: every byte of the stream belongs to exactly one frame (no prefix bytes in a Section 5 stream).
  uint64_t total = 0;
  for (const auto* f : *si.frames()) {
    total += f->size_bytes();
  }
  CHECK(total == stream.size());

  // Chapter 6 §6.2.6: no container and no timing_info: pts counts display slots, dts decode slots, in 1/1.
  CHECK(codes(si) == std::vector<std::string>{"SYN-GEN-0104"});
  CHECK(key.timebase_num() == 1);
  CHECK(key.timebase_den() == 1);
  CHECK(si.frames()->Get(3)->pts() == 1);
  CHECK(si.frames()->Get(3)->dts() == 3);
  CHECK(hidden.pts() == 1);  // the next output frame's
  CHECK(existing.pts() == 6);

  SECTION("determinism (§0.7.5): the same units give the same bytes") {
    CHECK(index_obu(stream) == index);
  }
}

TEST_CASE("index_builder_av1: frame header and tile group OBUs, metadata", "[index][av1][fast]") {
  // The same GOP re-muxed: every OBU_FRAME split into OBU_FRAME_HEADER + OBU_TILE_GROUP, and an HDR CLL metadata
  // OBU before the second frame (FFmpeg decodes the result).
  const Bytes stream = seed("seed_1_split_frames_metadata.bin");
  const Bytes index = index_obu(stream);
  check_valid(index);
  const fb::StreamIndex& si = root(index);
  CHECK(frame_tokens(si) == "K0 P1h P2h P3 P4 P5=2 P6 P7 P8=1");
  CHECK(si.display_order()->size() == 7);
  CHECK(codes(si) == std::vector<std::string>{"SYN-GEN-0104"});
  const fb::FrameInfo& key = *si.frames()->Get(0);
  CHECK(key.slice_count() == 1);
  REQUIRE(key.units()->size() == 4);  // TD, sequence header, frame header, tile group
  CHECK(si.units()->Get(key.units()->Get(3))->type_name()->string_view() == "OBU_TILE_GROUP");
  REQUIRE(si.sei()->size() == 1);
  const fb::Unit& md = *si.sei()->Get(0);
  CHECK(md.type_name()->string_view() == "OBU_METADATA");
  CHECK(md.frame()->decode_index() == 1);  // pending non-VCL unit: owned by the frame that follows it
  uint64_t total = 0;
  for (const auto* f : *si.frames()) {
    total += f->size_bytes();
  }
  CHECK(total == stream.size());
}

TEST_CASE("index_builder_av1: damaged streams still give an index", "[index][av1][fast]") {
  const Bytes stream = seed("seed_0_altref_gop.bin");

  SECTION("empty input: SYN-GEN-0101") {
    const Bytes index = index_obu({});
    check_valid(index);
    CHECK(root(index).frames()->empty());
    CHECK(has_code(root(index), "SYN-GEN-0101"));
  }
  SECTION("frames before the first sequence header belong to no frame (SYN-GEN-0102, once)") {
    // Drop the sequence header (unit 1): nothing can be parsed, every OBU is indexed as a unit.
    const Bytes whole_index = index_obu(stream);
    const fb::StreamIndex& whole = root(whole_index);
    const auto* sh = whole.units()->Get(1)->file_span();
    Bytes cut(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(sh->offset()));
    cut.insert(cut.end(), stream.begin() + static_cast<std::ptrdiff_t>(sh->offset() + sh->length()), stream.end());
    const Bytes index = index_obu(cut);
    check_valid(index);
    const fb::StreamIndex& si = root(index);
    CHECK(si.units()->size() == whole.units()->size() - 1);
    CHECK(si.frames()->empty());
    size_t warned = 0;
    for (const std::string& c : codes(si)) {
      warned += c == "SYN-GEN-0102" ? 1U : 0U;
    }
    CHECK(warned == 1);
  }
  SECTION("a frame header cut short: its unit carries the ParseError, the other frames are complete") {
    // Truncate the stream inside the header of the last OBU (the show_existing_frame header is 1 payload byte).
    const Bytes whole_index = index_obu(stream);
    const fb::StreamIndex& whole = root(whole_index);
    const fb::Unit& last = *whole.units()->Get(whole.units()->size() - 1);
    REQUIRE(last.type_name()->string_view() == "OBU_FRAME_HEADER");
    Bytes cut(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(last.file_span()->offset() + 2));
    cut[last.file_span()->offset() + 1] = 0;  // obu_size = 0: no payload
    const Bytes index = index_obu(cut);
    check_valid(index);
    const fb::StreamIndex& si = root(index);
    CHECK(si.frames()->size() == whole.frames()->size() - 1);
    const fb::Unit& bad = *si.units()->Get(si.units()->size() - 1);
    REQUIRE(bad.diagnostics() != nullptr);
    CHECK(bad.diagnostics()->Get(0)->code()->string_view() == "BR-0001");
    CHECK(bad.frame()->decode_index() == -1);
    CHECK(has_code(si, "BR-0001"));
  }
  SECTION("every prefix of the stream indexes without throwing and validates") {
    for (size_t n = 0; n < stream.size(); ++n) {
      const Bytes index = index_obu(std::span<const uint8_t>(stream.data(), n));
      CAPTURE(n);
      check_valid(index);
    }
  }
}

TEST_CASE("index_builder: a codec without a dispatch is refused", "[index][fast]") {
  CHECK_THROWS_AS(idx::StreamIndexBuilder(idx::BuilderOptions{fb::Codec::VP9}), idx::UnsupportedCodec);
}

// ---- AV1 conformance vectors (need the corpus) ------------------------------------------------------------------

// AT2: Given an AV1 stream with hidden frames, When indexed, Then frames.size() == shown + hidden and
// display_order.size() == shown.
TEST_CASE("index_av1_hidden_frames: frames = shown + hidden, display_order = shown",
          "[index][av1][fast][needs-corpus]") {
  struct Vector {
    std::string_view id;
    size_t frames;  // frame headers in the stream
    size_t shown;   // ffprobe -count_frames (one per temporal unit and spatial layer)
    size_t hidden;
  };
  // av1-1-b8-05-mv: KEY shown; INTER hidden (showable); INTER shown; INTER shown; show_existing_frame of slot 6
  // (trace_headers, as in av1_index_frame_count_vs_oracle). av1-1-b8-22-svc-L2T2: 8 temporal units of two spatial
  // layers, each layer's frame has show_frame = 1 (operating point selection is WI-1.124).
  const std::array<Vector, 5> vectors = {{{"aom_av1_1_b8_05_mv", 5, 4, 1},
                                          {"aom_av1_1_b8_06_mfmv", 4, 4, 0},
                                          {"aom_av1_1_b8_01_size_16x16", 2, 2, 0},
                                          {"aom_av1_1_b8_02_allintra", 39, 39, 0},
                                          {"aom_av1_1_b8_22_svc_L2T2", 16, 16, 0}}};
  for (const Vector& v : vectors) {
    CAPTURE(v.id);
    const Bytes index = index_ivf(ovt::test::corpus_or_skip(v.id));
    check_valid(index);
    const fb::StreamIndex& si = root(index);
    CHECK(si.frames()->size() == v.frames);
    CHECK(count_shown(si) == v.shown);
    CHECK(si.frames()->size() == count_shown(si) + v.hidden);
    CHECK(si.display_order()->size() == v.shown);
    CHECK(codes(si).empty());
  }
  const Bytes index = index_ivf(ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv"));
  const fb::StreamIndex& si = root(index);
  CHECK(frame_tokens(si) == "K0 P1h P2 P3 P4=1");
  // IVF timing: the hidden frame and the frame shown with it share the temporal unit's timestamp.
  CHECK(si.frames()->Get(1)->pts() == si.frames()->Get(2)->pts());
  CHECK(si.frames()->Get(4)->pts() == 3);
  CHECK(si.frames()->Get(0)->timebase_den() == 30);
  CHECK(si.units()->Get(0)->sample_index() == 0);
  // size_bytes sums to the IVF payload bytes (frame headers of the container excluded).
  uint64_t total = 0;
  for (const auto* f : *si.frames()) {
    total += f->size_bytes();
  }
  const auto file_size = std::filesystem::file_size(ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv"));
  CHECK(total == file_size - 32 - uintmax_t{12} * 4);
}

TEST_CASE("index_builder_av1: a truncated vector keeps its complete frames", "[index][av1][fast][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv");
  const Bytes whole_index = index_ivf(path);
  const fb::StreamIndex& whole = root(whole_index);
  REQUIRE(whole.frames()->size() == 5);
  // Cut into the last temporal unit's frame: the scanner reports the short sample, the frames before it stay.
  const Bytes index = index_ivf(path, 2000);
  check_valid(index);
  const fb::StreamIndex& si = root(index);
  CHECK(has_code(si, "IO-0001"));
  CHECK(si.frames()->size() >= 3);
  for (uint32_t i = 0; i < 3; ++i) {
    CHECK(si.frames()->Get(i)->size_bytes() == whole.frames()->Get(i)->size_bytes());
    CHECK(si.frames()->Get(i)->type() == whole.frames()->Get(i)->type());
  }
}
