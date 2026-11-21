// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the AV1 slot model integration of the StreamIndex builder (chapter 7 §7.4.3, §7.4.4, WI-1.124):
// declared_refs and dpb_after in the FrameInfo, the selected operating point (§7.3.6 row 1 drop rule, §7.4.3 last
// row) and the Annex B temporal-unit check (§7.3.9, SYN-AV1-0032). The corpus-free cases use the libaom-encoded
// 16x16 seeds of core/tests/fuzz/corpus/index_builder; the [needs-corpus] cases use the libaom test vectors.

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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/ivf.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "ovt/index.h"
#include "ovt/status.h"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

namespace fb = ovt::fb;
namespace idx = ovt::index;
using Bytes = std::vector<uint8_t>;

constexpr std::array<uint8_t, 32> kHash = {4, 5, 6};
constexpr std::string_view kNotInOperatingPoint = "SYN-AV1-0002";
constexpr std::string_view kAnnexBTemporalUnit = "SYN-AV1-0032";

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

const fb::StreamIndex& root(const Bytes& index) {
  return *flatbuffers::GetRoot<fb::StreamIndex>(index.data());
}

void check_valid(const Bytes& index) {
  for (const auto& v : ovt::model::validate_stream_index(index)) {
    FAIL_CHECK(v.rule << ": " << v.message << " at " << v.path);
  }
}

size_t count_code(const fb::StreamIndex& si, std::string_view code) {
  if (si.diagnostics() == nullptr) {
    return 0;
  }
  return static_cast<size_t>(std::ranges::count_if(
      *si.diagnostics(), [code](const fb::Diagnostic* d) { return d->code()->string_view() == code; }));
}

bool unit_has_code(const fb::Unit& u, std::string_view code) {
  return u.diagnostics() != nullptr && std::ranges::any_of(*u.diagnostics(), [code](const fb::Diagnostic* d) {
           return d->code()->string_view() == code;
         });
}

std::vector<int32_t> vec(const flatbuffers::Vector<int32_t>* v) {
  return v == nullptr ? std::vector<int32_t>{} : std::vector<int32_t>(v->begin(), v->end());
}

size_t count_shown(const fb::StreamIndex& si) {
  return static_cast<size_t>(std::ranges::count_if(*si.frames(), [](const fb::FrameInfo* f) { return f->shown(); }));
}

// The OBUs of an IVF file, one temporal unit per IVF frame.
std::vector<Bytes> ivf_temporal_units(const std::filesystem::path& path) {
  const Bytes data = read_file(path);
  const ovt::io::MemoryFile file(data, path.filename().string());
  const auto ivf = ovt::container::ivf::parse(file);
  REQUIRE(ivf.has_value());
  std::vector<Bytes> out;
  for (const auto& fr : ivf->frames) {
    REQUIRE(fr.size == fr.declared_size);
    const auto* begin = data.data() + fr.payload_offset();
    out.emplace_back(begin, begin + fr.size);
  }
  return out;
}

// The OBUs of one temporal unit of a Section 5 stream (every OBU carries obu_size).
std::vector<Bytes> split_obus(std::span<const uint8_t> tu) {
  std::vector<Bytes> out;
  ovt::syntax::scan_obu_low_overhead(
      tu, 0, ovt::syntax::kNoContainerSample, ovt::syntax::ObuSpanKind::Section5Stream,
      [&out](const ovt::syntax::ScannedUnit& u) { out.emplace_back(u.bytes.begin(), u.bytes.end()); });
  return out;
}

// Splits a Section 5 stream into temporal units: a new one starts at each temporal delimiter OBU.
std::vector<Bytes> section5_temporal_units(const Bytes& stream) {
  std::vector<Bytes> out;
  for (const Bytes& obu : split_obus(stream)) {
    const unsigned type = (obu[0] >> 3U) & 0x0FU;
    if (type == 2 || out.empty()) {
      out.emplace_back();
    }
    out.back().insert(out.back().end(), obu.begin(), obu.end());
  }
  return out;
}

void put_leb128(Bytes& out, uint64_t v) {
  do {
    uint8_t byte = v & 0x7FU;
    v >>= 7U;
    if (v != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (v != 0);
}

// AV1 Annex B (B.2) of a sequence of temporal units: temporal_unit(temporal_unit_size) { frame_unit(frame_unit_size)
// { obu_length, open_bitstream_unit } }. A frame unit ends before the second frame header of the temporal unit's
// OBUs; the OBUs keep their obu_size fields (obu_has_size_field may be 1 in Annex B).
Bytes to_annexb(const std::vector<Bytes>& temporal_units) {
  Bytes out;
  for (const Bytes& tu : temporal_units) {
    std::vector<Bytes> frame_units(1);
    bool has_frame = false;
    for (const Bytes& obu : split_obus(tu)) {
      const unsigned type = (obu[0] >> 3U) & 0x0FU;
      const bool frame_start = type == 3 || type == 6;  // OBU_FRAME_HEADER, OBU_FRAME
      if (frame_start && has_frame) {
        frame_units.emplace_back();
      }
      has_frame = has_frame || frame_start;
      put_leb128(frame_units.back(), obu.size());
      frame_units.back().insert(frame_units.back().end(), obu.begin(), obu.end());
    }
    Bytes body;
    for (const Bytes& fu : frame_units) {
      put_leb128(body, fu.size());
      body.insert(body.end(), fu.begin(), fu.end());
    }
    put_leb128(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
  }
  return out;
}

Bytes concat(const std::vector<Bytes>& parts) {
  Bytes out;
  for (const Bytes& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

idx::UnitInput input_of(const ovt::syntax::ScannedUnit& u) {
  idx::UnitInput in;
  in.file_offset = u.file_offset;
  in.bytes = u.bytes;
  in.prefix_span = u.prefix_span;
  in.sample_index = u.container_sample_index;
  in.temporal_unit_start = u.temporal_unit_start;
  return in;
}

// Indexes a Section 5 OBU stream with the given operating point.
Bytes index_section5(std::span<const uint8_t> stream, uint8_t operating_point = 0) {
  idx::BuilderOptions options{fb::Codec::AV1};
  options.av1_operating_point = operating_point;
  idx::StreamIndexBuilder b(options);
  ovt::syntax::scan_obu_low_overhead(
      stream, 0, ovt::syntax::kNoContainerSample, ovt::syntax::ObuSpanKind::Section5Stream,
      [&b](const ovt::syntax::ScannedUnit& u) { b.add_unit(input_of(u)); },
      [&b](const ovt::syntax::ScanDiagnostic& d) { b.add_scan_diagnostic(d); });
  const flatbuffers::DetachedBuffer out = b.finish(kHash);
  return Bytes(out.data(), out.data() + out.size());
}

// Indexes an Annex B OBU stream as ovt_stream_build_index() does (av1_annexb, temporal_unit_start from the scanner).
Bytes index_annexb(std::span<const uint8_t> stream) {
  idx::BuilderOptions options{fb::Codec::AV1};
  options.av1_annexb = true;
  idx::StreamIndexBuilder b(options);
  ovt::syntax::scan_obu_annexb(
      stream, 0, ovt::syntax::kNoContainerSample, [&b](const ovt::syntax::ScannedUnit& u) { b.add_unit(input_of(u)); },
      [&b](const ovt::syntax::ScanDiagnostic& d) { b.add_scan_diagnostic(d); });
  const flatbuffers::DetachedBuffer out = b.finish(kHash);
  return Bytes(out.data(), out.data() + out.size());
}

// ovt_stream_build_index() over an in-memory .obu file (the container module detects the framing).
Bytes index_c_api(std::span<const uint8_t> bytes) {
  ovt_File file;
  file.file = std::make_unique<ovt::io::MemoryFile>(bytes, "stream.obu");
  ovt_Container* container = nullptr;
  REQUIRE(ovt_container_open(&file, &container) == OVT_OK);
  ovt_Stream* stream = nullptr;
  REQUIRE(ovt_stream_open(container, 0, &stream) == OVT_OK);
  ovt_Buffer buf{nullptr, 0};
  const ovt_status st = ovt_stream_build_index(stream, nullptr, &buf);
  // This test links the module objects, not the shared library that exports ovt_buffer_free(): a caller-owned
  // buffer is a new[] array (core/src/api/buffer.cpp), released here the way ovt_buffer_free() does.
  const std::unique_ptr<const uint8_t[]> owner(buf.data);  // NOLINT(*-avoid-c-arrays)
  ovt_stream_close(stream);
  ovt_container_close(container);
  REQUIRE(st == OVT_OK);
  return {buf.data, buf.data + buf.size};
}

// Frame-level summary used to compare two indices of the same stream: type, shown, show_existing_of, poc, refs.
std::string frames_summary(const fb::StreamIndex& si) {
  std::string out;
  for (const auto* f : *si.frames()) {
    out += std::to_string(static_cast<int>(f->type())) + (f->shown() ? "s" : "h") +
           std::to_string(f->show_existing_of()) + "/" + std::to_string(f->poc()) + "[";
    for (const int32_t r : vec(f->declared_refs())) {
      out += std::to_string(r) + ",";
    }
    out += "|";
    for (const int32_t r : vec(f->dpb_after())) {
      out += std::to_string(r) + ",";
    }
    out += "] ";
  }
  return out;
}

// §7.4.4 holds frame by frame: declared_refs name earlier frames, dpb_after has 8 slots that hold -1 or a frame
// decoded up to this one, and a non-show_existing frame's references are all in the previous frame's dpb_after.
void check_slot_invariants(const fb::StreamIndex& si) {
  std::vector<int32_t> previous(8, -1);
  for (uint32_t i = 0; i < si.frames()->size(); ++i) {
    CAPTURE(i);
    const fb::FrameInfo& f = *si.frames()->Get(i);
    const std::vector<int32_t> dpb = vec(f.dpb_after());
    REQUIRE(dpb.size() == 8);
    for (const int32_t d : dpb) {
      CHECK(d >= -1);
      CHECK(d <= static_cast<int32_t>(i));
    }
    for (const int32_t r : vec(f.declared_refs())) {
      CHECK(r >= 0);
      CHECK(r < static_cast<int32_t>(i));
      CHECK(std::ranges::find(previous, r) != previous.end());
    }
    if (f.show_existing_of() >= 0) {
      CHECK(f.declared_refs() == nullptr);
      CHECK(std::ranges::find(previous, f.show_existing_of()) != previous.end());
    }
    previous = dpb;
  }
}

}  // namespace

// ---- §7.4.4 declared_refs / dpb_after (corpus-free) ------------------------------------------------------------

TEST_CASE("index_av1_slots_in_frameinfo: altref GOP declared_refs and dpb_after", "[index][av1][fast]") {
  const Bytes index = index_section5(seed("seed_0_altref_gop.bin"));
  check_valid(index);
  const fb::StreamIndex& si = root(index);
  REQUIRE(si.frames()->size() == 9);  // "K0 P1h P2h P3 P4 P5=2 P6 P7 P8=1" (test_index_builder.cpp)
  check_slot_invariants(si);
  // The shown KEY frame refreshes every slot.
  CHECK(vec(si.frames()->Get(0)->dpb_after()) == std::vector<int32_t>(8, 0));
  // A hidden frame keeps display_index -1 and is in its own dpb_after (§7.4.3 first row).
  const fb::FrameInfo& hidden = *si.frames()->Get(1);
  CHECK_FALSE(hidden.shown());
  CHECK(hidden.id()->display_index() == -1);
  const std::vector<int32_t> hidden_dpb = vec(hidden.dpb_after());
  CHECK(std::ranges::find(hidden_dpb, 1) != hidden_dpb.end());
  CHECK(vec(hidden.declared_refs()) == std::vector<int32_t>{0});
  // show_existing_frame of a non-KEY slot: no references, slots unchanged (§7.4.3 second row).
  for (const uint32_t e : {5U, 8U}) {
    CAPTURE(e);
    const fb::FrameInfo& existing = *si.frames()->Get(e);
    CHECK(existing.declared_refs() == nullptr);
    CHECK(vec(existing.dpb_after()) == vec(si.frames()->Get(e - 1)->dpb_after()));
  }
  CHECK(si.frames()->Get(5)->show_existing_of() == 2);
  CHECK(si.frames()->Get(8)->show_existing_of() == 1);
  CHECK(count_code(si, "SYN-AV1-0102") == 0);
  CHECK(count_code(si, kNotInOperatingPoint) == 0);
}

TEST_CASE("index_av1_slots_in_frameinfo: deterministic", "[index][av1][fast]") {
  const Bytes s = seed("seed_1_split_frames_metadata.bin");
  CHECK(index_section5(s) == index_section5(s));
  check_slot_invariants(root(index_section5(s)));
}

// ---- Acceptance test 3: Annex B vs Section 5 (corpus-free) -----------------------------------------------------

TEST_CASE("index_av1_annexb_vs_section5: unit and frame counts match", "[index][av1][fast]") {
  for (const std::string_view name : {"seed_0_altref_gop.bin", "seed_1_split_frames_metadata.bin"}) {
    CAPTURE(name);
    const Bytes section5 = seed(name);
    const Bytes annexb = to_annexb(section5_temporal_units(section5));
    const Bytes a = index_section5(section5);
    const Bytes b = index_annexb(annexb);
    check_valid(b);
    const fb::StreamIndex& sa = root(a);
    const fb::StreamIndex& sb = root(b);
    CHECK(sb.units()->size() == sa.units()->size());
    CHECK(sb.frames()->size() == sa.frames()->size());
    CHECK(frames_summary(sb) == frames_summary(sa));
    CHECK(count_code(sb, kAnnexBTemporalUnit) == 0);
    // The length fields are prefix bytes of the frames: every byte of the file belongs to a frame (S7).
    uint64_t total = 0;
    for (const auto* f : *sb.frames()) {
      total += f->size_bytes();
    }
    CHECK(total == annexb.size());
    // Through the C API: the container module detects the Annex B framing and the result is the same.
    const Bytes c = index_c_api(annexb);
    const fb::StreamIndex& sc = root(c);
    CHECK(sc.units()->size() == sa.units()->size());
    CHECK(frames_summary(sc) == frames_summary(sa));
    CHECK(count_code(sc, kAnnexBTemporalUnit) == 0);
  }
}

TEST_CASE("index_av1_annexb_temporal_unit_check: temporal_unit_size off the temporal delimiters is SYN-AV1-0032",
          "[index][av1][fast]") {
  const std::vector<Bytes> tus = section5_temporal_units(seed("seed_0_altref_gop.bin"));
  REQUIRE(tus.size() >= 4);
  // Temporal units 1 and 2 in one temporal_unit(): the second temporal delimiter is inside a temporal unit.
  std::vector<Bytes> merged{tus[0], concat({tus[1], tus[2]})};
  merged.insert(merged.end(), tus.begin() + 3, tus.end());
  const Bytes index = index_annexb(to_annexb(merged));
  check_valid(index);
  CHECK(count_code(root(index), kAnnexBTemporalUnit) == 1);
  // A temporal unit without its temporal delimiter: the temporal unit starts with another OBU.
  std::vector<Bytes> no_td = tus;
  const std::vector<Bytes> obus = split_obus(no_td[1]);
  REQUIRE(((obus[0][0] >> 3U) & 0x0FU) == 2U);
  no_td[1] = concat(std::vector<Bytes>(obus.begin() + 1, obus.end()));
  const Bytes index2 = index_annexb(to_annexb(no_td));
  CHECK(count_code(root(index2), kAnnexBTemporalUnit) == 1);
  // The same OBUs as a Section 5 stream are not checked.
  CHECK(count_code(root(index_section5(concat(no_td))), kAnnexBTemporalUnit) == 0);
}

// ---- libaom test vectors (need the corpus) ----------------------------------------------------------------------

// Acceptance test 1 (#731: operating point 0 of this stream, 0x303, selects every layer, so the drop is exercised
// with operating point 3, 0x101 = spatial layer 0, temporal layer 0; WI-1.116 AT3 / #322 did the same).
TEST_CASE("index_av1_operating_point: L2T2 frames outside the operating point are hidden with SYN-AV1-0002",
          "[index][av1][fast][needs-corpus]") {
  const Bytes stream = concat(ivf_temporal_units(ovt::test::corpus_or_skip("aom_av1_1_b8_22_svc_L2T2")));

  const Bytes all = index_section5(stream, 0);
  check_valid(all);
  const fb::StreamIndex& si_all = root(all);
  REQUIRE(si_all.frames()->size() == 16);
  CHECK(count_shown(si_all) == 16);
  CHECK(count_code(si_all, kNotInOperatingPoint) == 0);

  const Bytes base = index_section5(stream, 3);
  check_valid(base);
  const fb::StreamIndex& si = root(base);
  REQUIRE(si.frames()->size() == 16);
  size_t layer1 = 0;
  size_t hidden = 0;
  for (uint32_t i = 0; i < si.frames()->size(); ++i) {
    CAPTURE(i);
    const fb::FrameInfo& f = *si.frames()->Get(i);
    const bool outside = f.layer_id() == 1 || f.temporal_id() == 1;
    layer1 += f.layer_id() == 1 ? 1U : 0U;
    hidden += outside ? 1U : 0U;
    CHECK(f.shown() == !outside);
    if (f.layer_id() == 1) {
      CHECK_FALSE(f.shown());  // the acceptance test's statement
    }
    // The Info sits on the unit that starts the frame (its first frame or frame header OBU), and only there.
    size_t flagged = 0;
    bool start_flagged = false;
    bool seen_start = false;
    for (const uint32_t u : *f.units()) {
      const fb::Unit& unit = *si.units()->Get(u);
      const bool has = unit_has_code(unit, kNotInOperatingPoint);
      flagged += has ? 1U : 0U;
      if (!seen_start && (unit.type() == 3 || unit.type() == 6)) {  // OBU_FRAME_HEADER, OBU_FRAME
        seen_start = true;
        start_flagged = has;
      }
    }
    CHECK(start_flagged == outside);
    CHECK(flagged == (outside ? 1U : 0U));
    if (outside) {
      CHECK(f.id()->display_index() == -1);
    }
  }
  CHECK(layer1 == 8);
  CHECK(count_code(si, kNotInOperatingPoint) == hidden);
  CHECK(si.display_order()->size() == size_t{16} - hidden);
  // The slot model still runs over every frame: references and slots equal the all-layers index.
  for (uint32_t i = 0; i < si.frames()->size(); ++i) {
    CHECK(vec(si.frames()->Get(i)->declared_refs()) == vec(si_all.frames()->Get(i)->declared_refs()));
    CHECK(vec(si.frames()->Get(i)->dpb_after()) == vec(si_all.frames()->Get(i)->dpb_after()));
  }
  // An operating point beyond operating_points_cnt_minus_1 (3) selects operating point 0.
  CHECK(index_section5(stream, 9) == all);
}

// Acceptance test 2. libaom `inspect` needs CONFIG_INSPECTION=1, which CI does not build (#488), so the
// referenceFrame → slot map comes from FFmpeg trace_headers (ffmpeg -bsf:v trace_headers, FFmpeg 6.1):
//   frame 0 KEY shown;                 frame 1 INTER refresh_frame_flags 0x04, ref_frame_idx 0 1 2 3 4 5 6;
//   frame 2 INTER refresh 0x02, ref_frame_idx 2 0 1 3 4 5 6;   frame 3 INTER refresh 0x01, ref_frame_idx 1 2 0 3 4 5 6.
// Slots after each frame, by hand: [0 0 0 0 0 0 0 0], [0 0 1 0 0 0 0 0], [0 2 1 0 0 0 0 0], [3 2 1 0 0 0 0 0].
TEST_CASE("index_av1_declared_refs: av1-1-b8-06-mfmv refs match the reference slot map",
          "[index][av1][fast][needs-corpus]") {
  const Bytes stream = concat(ivf_temporal_units(ovt::test::corpus_or_skip("aom_av1_1_b8_06_mfmv")));
  const Bytes index = index_section5(stream);
  check_valid(index);
  const fb::StreamIndex& si = root(index);
  REQUIRE(si.frames()->size() == 4);
  check_slot_invariants(si);
  const std::array<std::vector<int32_t>, 4> refs{{{}, {0}, {1, 0}, {2, 1, 0}}};
  const std::array<std::vector<int32_t>, 4> dpb{
      {{0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0, 0, 0}, {0, 2, 1, 0, 0, 0, 0, 0}, {3, 2, 1, 0, 0, 0, 0, 0}}};
  for (uint32_t i = 0; i < 4; ++i) {
    CAPTURE(i);
    CHECK(vec(si.frames()->Get(i)->declared_refs()) == refs.at(i));
    CHECK(vec(si.frames()->Get(i)->dpb_after()) == dpb.at(i));
  }
  CHECK(count_code(si, "SYN-AV1-0102") == 0);
}

// Acceptance test 3 on the libaom vectors, plus the slot invariants on every stream of the WI's list.
TEST_CASE("index_av1_annexb_vs_section5: libaom vectors", "[index][av1][fast][needs-corpus]") {
  for (const std::string_view id : {"aom_av1_1_b8_05_mv", "aom_av1_1_b8_06_mfmv", "aom_av1_1_b8_22_svc_L1T2",
                                    "aom_av1_1_b8_22_svc_L2T1", "aom_av1_1_b8_22_svc_L2T2"}) {
    CAPTURE(id);
    const std::vector<Bytes> tus = ivf_temporal_units(ovt::test::corpus_or_skip(id));
    const Bytes a = index_section5(concat(tus));
    const Bytes b = index_annexb(to_annexb(tus));
    check_valid(a);
    check_valid(b);
    check_slot_invariants(root(a));
    CHECK(root(b).units()->size() == root(a).units()->size());
    CHECK(root(b).frames()->size() == root(a).frames()->size());
    CHECK(frames_summary(root(b)) == frames_summary(root(a)));
    CHECK(count_code(root(b), kAnnexBTemporalUnit) == 0);
  }
}
